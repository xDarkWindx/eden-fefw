// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The writer, the Gather and the applier of docs/list-diag-design.md 3.3, around N managers
// (one per core) and the ledger: CollectObserved (core/memory.cpp:765), GatherAll and
// ApplyBatchRanges (the diagnostic branch of InvalidateGPUCache, gpu.cpp:123-163), NoteApply
// (top of CachedWriteMemory) and Pop (the loop of PopAsyncBuffers, buffer_cache.h:719-722).
// `cache_mutex` stands for buffer_cache.mutex: the pop and every application take it, and the
// ledger code adds no other edge between them.
#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "ledger.h"
#include "manager.h"

namespace diag {

struct Range {
    u64 addr = 0;
    u64 size = 0;
    u32 mask = 0; // cores whose lot contributed to this range
};

struct Batch {
    std::vector<Range> pre;    // what the callbacks delivered, with their core
    std::vector<Range> merged; // after the join of gpu.cpp:138-161, with the union of masks
    std::vector<u64> lots;     // per core: the lot that core's Gather took (0: none)
};

struct Claim {
    u64 lot = 0; // 0: ambiguous
    bool ambiguous = true;
    bool registered = false;
    Handle handle;
};

enum class Sample { None, Equal, Different, Unverified };

struct Event {
    u32 core = 0;
    u64 lot = 0;
    u64 notice_addr = 0;
    u32 notice_size = 0;
    bool popped = false;
    Sample sample = Sample::None;
    u64 sample_bytes = 0;
};

template <unsigned N>
class System {
public:
    System(unsigned slots_per_core = 64) : ledger{N, slots_per_core, counters, clock} {}

    Counters counters;
    std::atomic<u64> clock{0};
    std::array<Manager<true>, N> managers;
    Ledger ledger;
    std::mutex cache_mutex;

    // Knobs of the tests.
    bool naive_no_late_check = false; // skip the apply_started_lot check (shows the false window)
    int mutant_delta = 0;             // claim the wrong lot (shows what address-only checks miss)
    std::function<void(u64, u8*, size_t)> read_guest;
    std::function<void(u64, u32)> on_window_flag; // truth hook: the record flagged by a pop
    std::function<void(u64, u64)> on_range_applied;

    std::mutex events_mutex;
    std::vector<Event> events;
    std::array<std::atomic<u64>, N> gather_calls{};
    std::array<std::atomic<u64>, N> gather_empty{};

    void SetGates(Gates* g) {
        gates_ = g;
        ledger.gates = g;
        for (auto& m : managers) {
            m.gates = g;
        }
    }

    /// core/memory.cpp:765 with the diagnostic: reads (done, started) before and started after
    /// the real Collect; the notice belongs to lot done+1 only if no Gather was running before
    /// and none started meanwhile.
    Claim CollectObserved(unsigned core, u64 addr, u32 size) {
        auto& m = managers[core];
        const u64 d = m.lot_done.load(std::memory_order_seq_cst);
        const u64 s = m.lot_started.load(std::memory_order_seq_cst);
        Pass(gates_, "w.after_r0");
        m.Collect(addr, size);
        Pass(gates_, "w.after_collect");
        const u64 s2 = m.lot_started.load(std::memory_order_seq_cst);
        Claim claim;
        claim.ambiguous = !(s == d && s2 == s);
        claim.lot = claim.ambiguous ? 0 : static_cast<u64>(static_cast<long long>(d) + 1 + mutant_delta);
        Pass(gates_, "w.after_s2");
        claim.handle = ledger.Register(core, addr, size, claim.lot, claim.ambiguous);
        claim.registered = claim.handle.ok;
        if (!claim.registered) {
            return claim;
        }
        Inc(counters.notice_registered);
        if (claim.ambiguous) {
            Inc(counters.notice_ambiguous);
            return claim;
        }
        Pass(gates_, "w.after_publish");
        // Published late? The applier stores "applying up to lot L" before it sweeps, and this
        // load comes after the publication: seq_cst makes either the sweep see the record or
        // this load see the lot (docs/list-diag-design.md 3.3.1).
        const u64 applying = naive_no_late_check ? 0 : m.apply_started_lot.load(std::memory_order_seq_cst);
        const u64 t_write = clock.load(std::memory_order_relaxed);
        if (claim.lot != 0 && applying >= claim.lot) {
            const MarkResult r = ledger.Mark(*claim.handle.slot, claim.handle.gen16, MarkKind::Late, t_write);
            if (r == MarkResult::Ok) {
                Inc(counters.notice_late);
                ledger.Free(*claim.handle.slot, claim.handle.gen16);
            }
        } else {
            ledger.Mark(*claim.handle.slot, claim.handle.gen16, MarkKind::Checked, t_write);
        }
        return claim;
    }

    /// The Gather of every manager, as System::GatherGPUDirtyMemory does (core.cpp:644-646),
    /// with the lot of each. Does not apply anything.
    Batch GatherAll() {
        Batch batch;
        batch.lots.assign(N, 0);
        Inc(counters.invalidate_calls);
        for (unsigned core = 0; core < N; ++core) {
            u64 lot = 0;
            bool any = false;
            std::function<void(PAddr, size_t)> callback = [&](PAddr a, size_t s) {
                any = true;
                batch.pre.push_back(Range{a, s, 1u << core});
            };
            managers[core].Gather(callback, &lot);
            Inc(gather_calls[core]);
            if (!any) {
                Inc(gather_empty[core]);
            }
            batch.lots[core] = lot;
        }
        // The join of gpu.cpp:143-161, unchanged: what OnCachedWrites receives.
        std::vector<std::pair<u64, u64>> ranges;
        for (const Range& r : batch.pre) {
            ranges.emplace_back(r.addr, r.size);
        }
        if (ranges.size() > 1) {
            std::sort(ranges.begin(), ranges.end());
            size_t out = 0;
            for (size_t in = 1; in < ranges.size(); ++in) {
                auto& last = ranges[out];
                const auto& [address, size] = ranges[in];
                const u64 last_end = last.first + last.second;
                if ((address >> 12) == (last.first >> 12) && address <= last_end) {
                    last.second = std::max(last_end, address + size) - last.first;
                } else {
                    ranges[++out] = ranges[in];
                }
            }
            ranges.resize(out + 1);
        }
        // The diagnostic, after the join: the union of the cores of the ranges inside each.
        for (const auto& [address, size] : ranges) {
            Range merged{address, size, 0};
            for (const Range& r : batch.pre) {
                if (r.addr >= address && r.addr + r.size <= address + size) {
                    merged.mask |= r.mask;
                }
            }
            batch.merged.push_back(merged);
        }
        return batch;
    }

    /// OnCachedWrites with the batch: stores "applying up to lot L" first, then applies each
    /// range under the cache mutex.
    void ApplyBatchRanges(const Batch& batch) {
        for (unsigned core = 0; core < N; ++core) {
            const u64 lot = batch.lots[core];
            if (lot == 0) {
                continue;
            }
            auto& m = managers[core];
            const u64 old = m.apply_started_lot.load(std::memory_order_seq_cst);
            if (lot <= old) {
                // Applications of one manager did not finish in order: the precondition of
                // docs/list-diag-design.md 3.3 was violated. Everything up to started is suspect.
                Inc(counters.apply_out_of_order);
                m.RaiseFloor(m.lot_started.load(std::memory_order_seq_cst));
            } else {
                m.apply_started_lot.store(lot, std::memory_order_seq_cst);
            }
        }
        Pass(gates_, "a.after_astore");
        for (const Range& r : batch.merged) {
            std::lock_guard lock{cache_mutex};
            NoteApply(r, batch);
            if (on_range_applied) {
                on_range_applied(r.addr, r.size);
            }
        }
    }

    void InvalidateDiag() {
        const Batch batch = GatherAll();
        ApplyBatchRanges(batch);
    }

    /// The loop of PopAsyncBuffers: `bytes` are the download bytes written over [start, end).
    void Pop(u64 start, u64 end, const u8* bytes) {
        std::lock_guard lock{cache_mutex};
        Inc(counters.pop_ranges);
        bool any = false;
        u64 flagged_bytes = 0;
        for (unsigned core = 0; core < N; ++core) {
            for (unsigned i = 0; i < ledger.SlotsPerCore(); ++i) {
                Slot& slot = ledger.SlotAt(core, i);
                View v;
                if (!ledger.Read(slot, v)) {
                    continue;
                }
                const u64 lo = std::max(start, v.addr);
                const u64 hi = std::min(end, v.addr + v.size);
                if (lo >= hi) {
                    continue;
                }
                if (Suspect(v)) {
                    Inc(counters.pop_over_ambiguous);
                    continue;
                }
                const u64 m = ledger.Marks(slot);
                if ((m & 0xffff) != v.gen16() || (m & (kBitApplied | kBitLate)) != 0) {
                    continue;
                }
                if ((m & kBitChecked) == 0) {
                    Inc(counters.pop_over_unchecked); // published, not yet checked: no claim
                    continue;
                }
                if ((m & kBitPopped) != 0) {
                    any = true;
                    flagged_bytes += hi - lo;
                    if (on_window_flag) on_window_flag(v.addr, v.size);
                    continue;
                }
                const u64 len = std::min<u64>(64, hi - lo);
                ledger.WritePop(slot, v.gen16(), lo, len, Fnv48(bytes + (lo - start), len));
                if (ledger.Mark(slot, v.gen16(), MarkKind::Popped, v.t_write) == MarkResult::Ok) {
                    any = true;
                    flagged_bytes += hi - lo;
                    Inc(counters.pop_over_notice_records);
                    if (on_window_flag) on_window_flag(v.addr, v.size);
                }
            }
        }
        if (any) {
            Inc(counters.pop_over_notice);
            Inc(counters.pop_over_notice_bytes, flagged_bytes);
        }
    }

    size_t PublishedSlots() {
        size_t n = 0;
        for (unsigned core = 0; core < N; ++core) {
            for (unsigned i = 0; i < ledger.SlotsPerCore(); ++i) {
                if ((ledger.SlotAt(core, i).state.load() & 3) == kPublished) {
                    ++n;
                }
            }
        }
        return n;
    }

    std::vector<Event> Events() {
        std::lock_guard lock{events_mutex};
        return events;
    }

private:
    bool Suspect(const View& v) const {
        return v.ambiguous ||
               (v.lot != 0 && v.lot <= managers[v.core].violation_floor.load(std::memory_order_seq_cst));
    }

    // Top of CachedWriteMemory: retire only the records whose (core, lot) is in the set of
    // this range; overlaps with another lot or an ambiguous lot are counted, not retired.
    void NoteApply(const Range& r, const Batch& batch) {
        for (unsigned core = 0; core < N; ++core) {
            for (unsigned i = 0; i < ledger.SlotsPerCore(); ++i) {
                Slot& slot = ledger.SlotAt(core, i);
                View v;
                if (!ledger.Read(slot, v)) {
                    continue;
                }
                if (v.addr + v.size <= r.addr || v.addr >= r.addr + r.size) {
                    continue;
                }
                if (Suspect(v)) {
                    Inc(counters.apply_overlap_ambiguous_lot);
                    continue;
                }
                const u64 m = ledger.Marks(slot);
                if ((m & 0xffff) != v.gen16() || (m & (kBitApplied | kBitLate)) != 0) {
                    continue;
                }
                const bool in_set = ((r.mask >> v.core) & 1) != 0 && v.lot == batch.lots[v.core];
                if (!in_set) {
                    Inc(counters.apply_overlap_other_lot);
                    continue;
                }
                if (v.addr < r.addr || v.addr + v.size > r.addr + r.size) {
                    Inc(counters.apply_partial);
                    continue;
                }
                Event ev;
                ev.core = v.core;
                ev.lot = v.lot;
                ev.notice_addr = v.addr;
                ev.notice_size = v.size;
                if ((m & kBitPopped) != 0) {
                    ev.popped = true;
                    const auto p = ledger.ReadPop(slot, v.gen16());
                    ev.sample = Sample::Unverified;
                    if (p.valid && p.len != 0 && read_guest) {
                        std::array<u8, 64> guest{};
                        read_guest(p.start, guest.data(), p.len);
                        ev.sample_bytes = p.len;
                        ev.sample = Fnv48(guest.data(), p.len) == p.hash ? Sample::Equal
                                                                         : Sample::Different;
                    }
                }
                if (ledger.Mark(slot, v.gen16(), MarkKind::Applied, v.t_write) != MarkResult::Ok) {
                    continue;
                }
                Inc(counters.notice_applied);
                if (ev.popped) {
                    switch (ev.sample) {
                    case Sample::Equal: Inc(counters.window_sample_equal); break;
                    case Sample::Different: Inc(counters.window_sample_different); break;
                    default: Inc(counters.window_unverified); break;
                    }
                }
                {
                    std::lock_guard lock{events_mutex};
                    events.push_back(ev);
                }
                ledger.Free(slot, v.gen);
            }
        }
    }

    Gates* gates_ = nullptr;
};

} // namespace diag
