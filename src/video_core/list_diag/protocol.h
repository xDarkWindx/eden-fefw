// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The protocol of the EDEN_LIST_DIAG diagnostic, parts `pop` and `blk` (docs/list-diag-design.md,
// 3.3 to 5): the notice registration with its lot, the link between a Gather and its application,
// the late-publication check, the pop and the application hooks, and the report.
//
// One implementation: the emulator includes this file, and tools/list-diag-harness includes the
// same file and drives it against the real Core::GPUDirtyMemoryManager. Nothing here takes a lock
// that orders the pop against the application of a notice; the pop and every application are
// already serialised by the buffer cache mutex, and this code adds no other edge between them.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/gpu_dirty_memory_manager.h"
#include "video_core/list_diag/counters.h"
#include "video_core/list_diag/hooks.h"
#include "video_core/list_diag/ledger.h"

namespace VideoCommon::ListDiag {

struct Range {
    u64 addr = 0;
    u64 size = 0;
    u32 mask = 0; ///< Cores whose lot contributed to this range
};

enum class Sample : u8 { None, Equal, Different, Unverified };

enum class EventKind : u8 { PopHit, PopApplied, PopNever, Blk, P0Violated };
constexpr unsigned kEventKinds = 5;

struct Event {
    EventKind kind = EventKind::PopHit;
    Sample sample = Sample::None;
    bool ambiguous = false;
    u32 core = 0;
    u64 lot = 0;
    u64 notice_addr = 0;
    u32 notice_size = 0;
    std::array<u64, 10> v{};
};

/// The first kLogFirst events of each kind are logged, then one in kLogEvery; never more than
/// kLogMax lines in the whole session (docs/list-diag-design.md, decision 6).
constexpr u64 kLogFirst = 32;
constexpr u64 kLogEvery = 64;
constexpr u64 kLogMax = 512;
constexpr unsigned kEventSlots = 256;

template <unsigned N, class Hooks = ProdHooks>
class Protocol {
    static_assert(N >= 1 && N <= kMaxCores);

public:
    struct Claim {
        u64 lot = 0; ///< 0: ambiguous
        bool ambiguous = true;
        bool registered = false;
        Handle handle;
    };
    struct Gathered {
        std::vector<Range> pre; ///< What the callbacks delivered, with their core
        std::array<u64, N> lots{};
    };
    struct Batch {
        std::vector<Range> pre;
        std::vector<Range> merged; ///< The ranges OnCachedWrites receives, with the union of cores
        std::array<u64, N> lots{};
    };

    explicit Protocol(unsigned slots_per_core = 1024) : ledger{N, slots_per_core, counters} {}

    Counters counters;
    Ledger<Hooks> ledger;
    std::array<Core::GatherLotState, N> lots{};
    bool pop_on = true;
    bool blk_on = true;

    // ---------------------------------------------------------------------------------------
    // core/memory.cpp:765: the Collect, observed.

    /// Reads (done, started) before and started after the real Collect: the notice belongs to lot
    /// done + 1 of this core only if no Gather was running before and none started meanwhile
    /// (docs/list-diag-design.md, 3.3). Registered AFTER the Collect and BEFORE the caller's
    /// memcpy: it is a registered notice, never a completed write (3.5).
    template <class Manager>
    Claim CollectObserved(Manager& manager, unsigned core, u64 addr, u32 size) {
        auto& st = lots[core];
        const u64 d = st.lot_done.load(std::memory_order_seq_cst);
        const u64 s = st.lot_started.load(std::memory_order_seq_cst);
        Hooks::At("w.after_r0");
        manager.template CollectT<true, Hooks>(addr, size);
        Hooks::At("w.after_collect");
        const u64 s2 = st.lot_started.load(std::memory_order_seq_cst);
        Claim claim;
        claim.ambiguous = !(s == d && s2 == s);
        claim.lot = claim.ambiguous ? 0 : d + 1;
        if constexpr (Hooks::kTest) {
            if (!claim.ambiguous) {
                claim.lot = static_cast<u64>(static_cast<long long>(claim.lot) + Hooks::MutantDelta());
            }
        }
        Hooks::At("w.after_s2");
        Reclaimed reclaimed;
        claim.handle = ledger.Register(core, addr, size, claim.lot, claim.ambiguous, &reclaimed);
        claim.registered = claim.handle.ok;
        if (reclaimed.happened && reclaimed.was_popped) {
            Event ev;
            ev.kind = EventKind::PopNever;
            ev.core = reclaimed.core;
            ev.lot = reclaimed.lot;
            ev.notice_addr = reclaimed.addr;
            ev.notice_size = reclaimed.size;
            Push(ev);
        }
        if (!claim.registered) {
            return claim;
        }
        Inc(counters.notice_registered);
        if (claim.ambiguous) {
            Inc(counters.notice_ambiguous);
            return claim;
        }
        Hooks::At("w.after_publish");
        // Published late? The applier stores "applying up to lot L" before it sweeps and this load
        // comes after the publication: with seq_cst either the sweep sees the record or this load
        // sees the lot (docs/list-diag-design.md, 3.3.1).
        u64 applying = st.apply_started_lot.load(std::memory_order_seq_cst);
        if constexpr (Hooks::kTest) {
            if (Hooks::NaiveNoLateCheck()) {
                applying = 0;
            }
        }
        const u64 t_write = Hooks::NowUs();
        if (claim.lot != 0 && applying >= claim.lot) {
            const MarkResult r = ledger.Mark(*claim.handle.slot, claim.handle.tag(), MarkKind::Late, t_write);
            if (r == MarkResult::Ok) {
                Inc(counters.notice_late);
                ledger.Free(core, *claim.handle.slot, claim.handle.gen);
            }
        } else {
            ledger.Mark(*claim.handle.slot, claim.handle.tag(), MarkKind::Checked, t_write);
        }
        return claim;
    }

    // ---------------------------------------------------------------------------------------
    // gpu.cpp InvalidateGPUCache, diagnostic branch.

    /// The Gather of every manager (the calls the emulator already makes, System::
    /// GatherGPUDirtyMemory, core/core.cpp:644-646), in the variant that also takes the lot.
    template <class Manager>
    void GatherAll(std::span<Manager> managers, Gathered& out) {
        out.pre.clear();
        out.lots.fill(0);
        Inc(counters.invalidate_calls);
        const size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const size_t previous = last_thread_.exchange(tid, std::memory_order_relaxed);
        if (previous != 0 && previous != tid) {
            Inc(counters.invalidate_thread_switches);
        }
        const unsigned count = static_cast<unsigned>(std::min<size_t>(N, managers.size()));
        for (unsigned core = 0; core < count; ++core) {
            u64 lot = 0;
            bool any = false;
            std::function<void(PAddr, size_t)> callback = [&](PAddr a, size_t s) {
                any = true;
                out.pre.push_back(Range{a, s, 1u << core});
            };
            managers[core].template GatherT<true, Hooks>(callback, &lots[core], &lot);
            Inc(counters.gather_calls[core]);
            if (!any) {
                Inc(counters.gather_empty[core]);
            }
            out.lots[core] = lot;
        }
        if (out.pre.empty()) {
            Inc(counters.invalidate_empty);
        }
        CheckViolation();
    }

    /// After the join of ranges (unchanged, gpu.cpp:143-161): the cores of each final range are the
    /// union of the cores of the delivered ranges inside it.
    Batch MakeBatch(Gathered&& gathered, const std::vector<std::pair<u64, u64>>& final_ranges) {
        Batch batch;
        batch.lots = gathered.lots;
        for (const auto& [address, size] : final_ranges) {
            Range merged{address, size, 0};
            for (const Range& r : gathered.pre) {
                if (r.addr >= address && r.addr + r.size <= address + size) {
                    merged.mask |= r.mask;
                }
            }
            batch.merged.push_back(merged);
        }
        batch.pre = std::move(gathered.pre);
        return batch;
    }

    /// Stores "applying up to lot L" for each manager BEFORE the sweep, and publishes the batch for
    /// the application hooks of this thread until EndApply.
    void BeginApply(const Batch& batch) {
        for (unsigned core = 0; core < N; ++core) {
            const u64 lot = batch.lots[core];
            if (lot == 0) {
                continue;
            }
            auto& st = lots[core];
            const u64 old = st.apply_started_lot.load(std::memory_order_seq_cst);
            if (lot <= old) {
                // Applications of one manager did not finish in order: precondition P0 was
                // violated. Everything up to started is suspect.
                Inc(counters.apply_out_of_order);
                Core::GPUDirtyMemoryManager::RaiseFloor(st, st.lot_started.load(std::memory_order_seq_cst));
            } else {
                st.apply_started_lot.store(lot, std::memory_order_seq_cst);
            }
        }
        Hooks::At("a.after_astore");
        current_batch_ = &batch;
        CheckViolation();
    }

    void EndApply() {
        current_batch_ = nullptr;
    }

    // ---------------------------------------------------------------------------------------
    // buffer_cache.h: the top of CachedWriteMemory.

    /// Retires the notices whose (core, lot) belongs to this range, and accounts problem B (the 64-byte
    /// block). `gpu_modified` and `cpu_read` are the buffer cache's ranges BEFORE the application
    /// subtracts anything from them. `read_guest(addr, out, len)` reads guest memory.
    template <class RangeSet, class ReadGuest>
    void NoteApply(u64 addr, u64 size, const RangeSet& gpu_modified, const RangeSet& cpu_read,
                   bool track_cpu_reads, ReadGuest&& read_guest) {
        const Batch* batch = current_batch_;
        const u64 end = addr + size;
        u32 mask = 0;
        bool linked = false;
        if (batch != nullptr) {
            for (const Range& r : batch->merged) {
                if (r.addr == addr && r.size == size) {
                    mask |= r.mask;
                    linked = true;
                }
            }
        }
        if (!linked) {
            Inc(counters.apply_unlinked);
        }
        std::vector<std::pair<u64, u64>> exact;
        bool ambiguous = false;
        ledger.ForEachLive([&](unsigned core, unsigned index) {
            Slot& slot = ledger.SlotAt(core, index);
            View v;
            if (!ledger.Read(slot, v)) {
                return;
            }
            if (v.addr + v.size <= addr || v.addr >= end) {
                return;
            }
            if (Suspect(v)) {
                Inc(counters.apply_overlap_ambiguous_lot);
                ambiguous = true;
                return;
            }
            const u64 m = ledger.Marks(slot);
            if ((m & 0xffff) != v.tag() || (m & (kBitApplied | kBitLate)) != 0) {
                return;
            }
            const bool in_set = linked && ((mask >> v.core) & 1) != 0 && v.lot == batch->lots[v.core];
            if (!in_set) {
                Inc(counters.apply_overlap_other_lot);
                ambiguous = true;
                return;
            }
            if (v.addr < addr || v.addr + v.size > end) {
                Inc(counters.apply_partial);
                ambiguous = true;
                return;
            }
            Retire(slot, v, m, read_guest);
            exact.emplace_back(v.addr, v.addr + v.size);
        });
        if (blk_on) {
            AccountBlock(addr, size, exact, ambiguous || !linked, gpu_modified, cpu_read, track_cpu_reads);
        }
    }

    // ---------------------------------------------------------------------------------------
    // buffer_cache.h: the loop of PopAsyncBuffers.

    /// `bytes` are the download bytes the pop is about to write over [start, end).
    void NotePop(u64 start, u64 end, const u8* bytes) {
        Inc(counters.pop_ranges);
        bool any = false;
        u64 flagged_bytes = 0;
        ledger.ForEachLive([&](unsigned core, unsigned index) {
            Slot& slot = ledger.SlotAt(core, index);
            View v;
            if (!ledger.Read(slot, v)) {
                return;
            }
            const u64 lo = std::max(start, v.addr);
            const u64 hi = std::min(end, v.addr + v.size);
            if (lo >= hi) {
                return;
            }
            if (Suspect(v)) {
                Inc(counters.pop_over_ambiguous); // may already be applied: no claim
                return;
            }
            const u64 m = ledger.Marks(slot);
            if ((m & 0xffff) != v.tag() || (m & (kBitApplied | kBitLate)) != 0) {
                return;
            }
            if ((m & kBitChecked) == 0) {
                Inc(counters.pop_over_unchecked); // published, not yet checked: no claim
                return;
            }
            if ((m & kBitPopped) != 0) {
                any = true;
                flagged_bytes += hi - lo;
                if constexpr (Hooks::kTest) {
                    Hooks::OnWindowFlag(v.addr, v.size);
                }
                return;
            }
            const u64 len = std::min<u64>(64, hi - lo);
            ledger.WritePop(slot, v.tag(), lo, len, Fnv48(bytes + (lo - start), len));
            if (ledger.Mark(slot, v.tag(), MarkKind::Popped, v.t_write) == MarkResult::Ok) {
                any = true;
                flagged_bytes += hi - lo;
                if constexpr (Hooks::kTest) {
                    Hooks::OnWindowFlag(v.addr, v.size);
                }
                Inc(counters.pop_over_notice_records);
                const u64 now = Hooks::NowUs();
                const u64 width = now >= v.t_write ? now - v.t_write : 0;
                Inc(counters.notice_to_pop_us_sum, width);
                Max(counters.notice_to_pop_us_max, width);
                Inc(counters.notice_to_pop_n);
                Event ev;
                ev.kind = EventKind::PopHit;
                ev.core = v.core;
                ev.lot = v.lot;
                ev.notice_addr = v.addr;
                ev.notice_size = v.size;
                ev.v[0] = start;
                ev.v[1] = end;
                ev.v[2] = hi - lo;
                ev.v[3] = width;
                Push(ev);
            }
        });
        if (any) {
            Inc(counters.pop_over_notice);
            Inc(counters.pop_over_notice_bytes, flagged_bytes);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Precondition P0 (one Gather at a time per manager, applications in the order of the takings).

    [[nodiscard]] bool P0Violated() const {
        return p0_violated_.load(std::memory_order_acquire);
    }

    /// On the first gather_overlapped or apply_out_of_order the report says so, once in the log,
    /// and the summary repeats it. The counters go on; a snapshot taken at that instant lets the
    /// reader tell what came before from what came after.
    void CheckViolation() {
        u64 overlapped = 0;
        for (unsigned core = 0; core < N; ++core) {
            overlapped += lots[core].gather_overlapped.load(std::memory_order_seq_cst);
        }
        counters.gather_overlapped.store(overlapped, std::memory_order_relaxed);
        if (overlapped == 0 && Get(counters.apply_out_of_order) == 0) {
            return;
        }
        bool expected = false;
        if (!p0_claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        auto fields = Fields(counters);
        at_violation_.clear();
        for (const FieldRef& f : fields) {
            at_violation_.push_back(Get(*f.counter));
        }
        p0_violated_.store(true, std::memory_order_release);
        Event ev;
        ev.kind = EventKind::P0Violated;
        ev.v[0] = overlapped;
        ev.v[1] = Get(counters.apply_out_of_order);
        PushForced(ev);
    }

    // ---------------------------------------------------------------------------------------
    // The log and the report.

    /// Formats and hands to `sink` the events queued so far. One caller at a time; others return.
    template <class Sink>
    unsigned Drain(Sink&& sink) {
        if (drain_busy_.exchange(true, std::memory_order_acquire)) {
            return 0;
        }
        unsigned lines = 0;
        for (;;) {
            EventSlot& slot = events_[tail_ % kEventSlots];
            if (slot.state.load(std::memory_order_acquire) != 2) {
                break;
            }
            const Event ev = slot.ev;
            slot.state.store(0, std::memory_order_release);
            ++tail_;
            sink(FormatEvent(ev));
            ++lines;
        }
        drain_busy_.store(false, std::memory_order_release);
        return lines;
    }

    /// At most once per second (microsecond clock of the hooks).
    template <class Sink>
    void MaybeDrain(Sink&& sink) {
        const u64 now = Hooks::NowUs();
        u64 last = last_drain_.load(std::memory_order_relaxed);
        if (now - last < 1'000'000 && last != 0) {
            return;
        }
        if (!last_drain_.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return;
        }
        CheckViolation();
        Drain(sink);
    }

    /// The lines of the periodic report (EDEN_BACK_THREAD_STATS): totals and the change per second
    /// since the previous call. One caller at a time.
    std::vector<std::string> ReportLines(double elapsed_seconds) {
        auto fields = Fields(counters);
        std::vector<u64> now;
        for (const FieldRef& f : fields) {
            now.push_back(Get(*f.counter));
        }
        if (last_report_.size() != now.size()) {
            last_report_.assign(now.size(), 0);
        }
        std::vector<std::string> lines;
        const char* prefix = P0Violated() ? "[ListDiag][P0 VIOLATED: results after this are not reliable] "
                                          : "[ListDiag] ";
        const Group groups[] = {Group::Notices, Group::Pop, Group::Apply, Group::Blk, Group::Calls, Group::Log};
        const char* names[] = {"notices", "pop", "apply", "blk", "calls", "log"};
        for (size_t g = 0; g < 6; ++g) {
            std::string line = std::string{prefix} + names[g] + ":";
            bool any = false;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].group != groups[g] || (now[i] == 0 && last_report_[i] == 0)) {
                    continue;
                }
                char buffer[96];
                const double rate = elapsed_seconds > 0 ? static_cast<double>(now[i] - last_report_[i]) / elapsed_seconds : 0;
                std::snprintf(buffer, sizeof(buffer), " %s=%llu(+%.1f/s)", fields[i].name,
                              static_cast<unsigned long long>(now[i]), rate);
                line += buffer;
                any = true;
            }
            if (g == 4) {
                for (unsigned core = 0; core < N; ++core) {
                    const u64 calls = Get(counters.gather_calls[core]);
                    if (calls != 0) {
                        char buffer[96];
                        std::snprintf(buffer, sizeof(buffer), " gather[%u]=%llu(empty %llu)", core,
                                      static_cast<unsigned long long>(calls),
                                      static_cast<unsigned long long>(Get(counters.gather_empty[core])));
                        line += buffer;
                        any = true;
                    }
                }
            }
            if (any) {
                lines.push_back(std::move(line));
            }
        }
        last_report_ = now;
        return lines;
    }

    /// The summary at the end of the session: every non-zero counter, the P0 notice if it applies,
    /// and the counters as they were when the violation was first seen.
    std::vector<std::string> SummaryLines() {
        CheckViolation();
        auto fields = Fields(counters);
        std::vector<std::string> lines;
        if (P0Violated()) {
            lines.push_back("[ListDiag][summary] *** precondicao P0 violada: os resultados do diagnostico a partir da primeira "
                            "violacao nao sao confiaveis (gather_overlapped=" +
                            std::to_string(Get(counters.gather_overlapped)) +
                            ", apply_out_of_order=" + std::to_string(Get(counters.apply_out_of_order)) + ") ***");
        }
        std::string line = "[ListDiag][summary]";
        for (const FieldRef& f : fields) {
            const u64 value = Get(*f.counter);
            if (value != 0) {
                line += std::string{" "} + f.name + "=" + std::to_string(value);
            }
        }
        lines.push_back(line);
        for (unsigned core = 0; core < N; ++core) {
            const u64 calls = Get(counters.gather_calls[core]);
            if (calls != 0) {
                lines.push_back("[ListDiag][summary] gather[" + std::to_string(core) + "]=" + std::to_string(calls) +
                                " empty=" + std::to_string(Get(counters.gather_empty[core])));
            }
        }
        if (P0Violated()) {
            std::string before = "[ListDiag][summary] counters at the first violation:";
            for (size_t i = 0; i < fields.size() && i < at_violation_.size(); ++i) {
                if (at_violation_[i] != 0) {
                    before += std::string{" "} + fields[i].name + "=" + std::to_string(at_violation_[i]);
                }
            }
            lines.push_back(before);
        }
        return lines;
    }

    std::string FormatEvent(const Event& ev) const {
        char b[512];
        const auto hex = [](u64 x) { return static_cast<unsigned long long>(x); };
        switch (ev.kind) {
        case EventKind::PopHit:
            std::snprintf(b, sizeof(b),
                          "[ListDiag][pop] hit: notice=[0x%llx,+%u) core=%u lot=%llu download=[0x%llx,0x%llx) overlap=%llu "
                          "order=notice -> pop@+%lluus (apply pending)",
                          hex(ev.notice_addr), ev.notice_size, ev.core, hex(ev.lot), hex(ev.v[0]), hex(ev.v[1]),
                          hex(ev.v[2]), hex(ev.v[3]));
            break;
        case EventKind::PopApplied: {
            const char* label = ev.sample == Sample::Equal       ? "sample_equal"
                                : ev.sample == Sample::Different ? "sample_different"
                                                                 : "unverified";
            std::snprintf(b, sizeof(b),
                          "[ListDiag][pop] applied: notice=[0x%llx,+%u) core=%u lot=%llu order=notice -> pop@+%lluus -> "
                          "apply@+%lluus sample=%s (sample=%lluB)",
                          hex(ev.notice_addr), ev.notice_size, ev.core, hex(ev.lot), hex(ev.v[0]), hex(ev.v[1]), label,
                          hex(ev.v[2]));
            break;
        }
        case EventKind::PopNever:
            std::snprintf(b, sizeof(b),
                          "[ListDiag][pop] never: notice=[0x%llx,+%u) core=%u lot=%llu was reached by a pop and expired "
                          "without being applied (apply=never)",
                          hex(ev.notice_addr), ev.notice_size, ev.core, hex(ev.lot));
            break;
        case EventKind::Blk: {
            char at_risk[48];
            if (ev.ambiguous) {
                std::snprintf(at_risk, sizeof(at_risk), "ambiguous");
            } else {
                std::snprintf(at_risk, sizeof(at_risk), "%llu", hex(ev.v[3]));
            }
            std::snprintf(b, sizeof(b),
                          "[ListDiag][blk] range=[0x%llx,+%llu) blocks=%llu/64 exact=%llu(first [0x%llx,+%llu)) "
                          "gpu_covered=%llu at_risk=%s (cpu_read=%llu not_seen_cpu_read=%llu unknown=%llu)",
                          hex(ev.v[0]), hex(ev.v[1]), hex((ev.v[1] + 63) / 64), hex(ev.v[7]), hex(ev.v[8]), hex(ev.v[9]),
                          hex(ev.v[2]), at_risk, hex(ev.v[4]), hex(ev.v[5]), hex(ev.v[6]));
            break;
        }
        case EventKind::P0Violated:
            std::snprintf(b, sizeof(b),
                          "[ListDiag] *** precondicao P0 violada: os resultados do diagnostico a partir daqui nao sao "
                          "confiaveis (gather_overlapped=%llu apply_out_of_order=%llu) ***",
                          hex(ev.v[0]), hex(ev.v[1]));
            break;
        }
        return std::string{b};
    }

private:
    bool Suspect(const View& v) const {
        return v.ambiguous ||
               (v.lot != 0 && v.lot <= lots[v.core].violation_floor.load(std::memory_order_seq_cst));
    }

    template <class ReadGuest>
    void Retire(Slot& slot, const View& v, u64 marks, ReadGuest& read_guest) {
        Sample sample = Sample::None;
        u64 sample_bytes = 0;
        const bool popped = (marks & kBitPopped) != 0;
        if (popped) {
            const auto p = ledger.ReadPop(slot, v.tag());
            sample = Sample::Unverified;
            if (p.valid && p.len != 0) {
                std::array<u8, 64> guest{};
                read_guest(p.start, guest.data(), static_cast<size_t>(p.len));
                sample_bytes = p.len;
                sample = Fnv48(guest.data(), static_cast<size_t>(p.len)) == p.hash ? Sample::Equal : Sample::Different;
            }
        }
        if (ledger.Mark(slot, v.tag(), MarkKind::Applied, v.t_write) != MarkResult::Ok) {
            return;
        }
        Inc(counters.notice_applied);
        const u64 now = Hooks::NowUs();
        const u64 width = now >= v.t_write ? now - v.t_write : 0;
        Inc(counters.notice_to_apply_us_sum, width);
        Max(counters.notice_to_apply_us_max, width);
        Inc(counters.notice_to_apply_n);
        if (popped) {
            switch (sample) {
            case Sample::Equal: Inc(counters.window_sample_equal); break;
            case Sample::Different: Inc(counters.window_sample_different); break;
            default: Inc(counters.window_unverified); break;
            }
            Inc(counters.window_sample_bytes, sample_bytes);
            Event ev;
            ev.kind = EventKind::PopApplied;
            ev.sample = sample;
            ev.core = v.core;
            ev.lot = v.lot;
            ev.notice_addr = v.addr;
            ev.notice_size = v.size;
            const u64 t_pop = ((marks >> kPopShift) & kTimeMax) * 2;
            ev.v[0] = t_pop;
            ev.v[1] = width;
            ev.v[2] = sample_bytes;
            Push(ev);
        }
        ledger.Free(v.core, slot, v.gen);
    }

    template <class RangeSet>
    void AccountBlock(u64 addr, u64 size, std::vector<std::pair<u64, u64>>& exact, bool ambiguous,
                      const RangeSet& gpu_modified, const RangeSet& cpu_read, bool track_cpu_reads) {
        const u64 end = addr + size;
        Inc(counters.blk_applies);
        Inc(counters.blk_bytes_range, size);
        std::vector<std::pair<u64, u64>> gpu;
        u64 covered = 0;
        gpu_modified.ForEachInRange(addr, size, [&](u64 s, u64 e) {
            s = std::max(s, addr);
            e = std::min(e, end);
            if (e > s) {
                gpu.emplace_back(s, e);
                covered += e - s;
            }
        });
        // The exact writes of the same lot, as a union.
        std::sort(exact.begin(), exact.end());
        std::vector<std::pair<u64, u64>> merged_exact;
        for (const auto& [s, e] : exact) {
            if (!merged_exact.empty() && s <= merged_exact.back().second) {
                merged_exact.back().second = std::max(merged_exact.back().second, e);
            } else {
                merged_exact.emplace_back(s, e);
            }
        }
        u64 exact_bytes = 0;
        for (const auto& [s, e] : merged_exact) {
            exact_bytes += e - s;
        }
        if (!ambiguous) {
            Inc(counters.blk_bytes_exact, exact_bytes);
        }
        if (covered == 0) {
            return;
        }
        Inc(counters.blk_applies_over_gpu);
        Inc(counters.blk_gpu_bytes_covered, covered);
        Event ev;
        ev.kind = EventKind::Blk;
        ev.v[0] = addr;
        ev.v[1] = size;
        ev.v[2] = covered;
        ev.v[7] = merged_exact.size();
        if (!merged_exact.empty()) {
            ev.v[8] = merged_exact.front().first;
            ev.v[9] = merged_exact.front().second - merged_exact.front().first;
        }
        if (ambiguous || merged_exact.empty()) {
            // No exact writes of this lot to subtract (or another lot or an ambiguous lot is in the
            // range): the result is "ambiguous", never a guess. `gpu_covered` stays an upper bound.
            Inc(counters.blk_ambiguous);
            ev.ambiguous = true;
            Push(ev);
            return;
        }
        // at_risk = the GPU bytes of the range that the CPU did not write.
        u64 at_risk = 0, cpu_read_bytes = 0;
        for (const auto& [gs, ge] : gpu) {
            u64 cursor = gs;
            const auto account = [&](u64 s, u64 e) {
                if (e <= s) {
                    return;
                }
                at_risk += e - s;
                if (track_cpu_reads) {
                    cpu_read.ForEachInRange(s, e - s, [&](u64 rs, u64 re) {
                        rs = std::max(rs, s);
                        re = std::min(re, e);
                        if (re > rs) {
                            cpu_read_bytes += re - rs;
                        }
                    });
                }
            };
            for (const auto& [es, ee] : merged_exact) {
                if (ee <= cursor) {
                    continue;
                }
                if (es >= ge) {
                    break;
                }
                account(cursor, std::min(es, ge));
                cursor = std::max(cursor, ee);
            }
            account(cursor, ge);
        }
        Inc(counters.blk_gpu_bytes_at_risk, at_risk);
        ev.v[3] = at_risk;
        if (track_cpu_reads) {
            const u64 not_seen = at_risk >= cpu_read_bytes ? at_risk - cpu_read_bytes : 0;
            Inc(counters.blk_at_risk_cpu_read, cpu_read_bytes);
            Inc(counters.blk_at_risk_not_seen_cpu_read, not_seen);
            ev.v[4] = cpu_read_bytes;
            ev.v[5] = not_seen;
        } else {
            Inc(counters.blk_at_risk_unknown, at_risk);
            ev.v[6] = at_risk;
        }
        if (at_risk != 0) {
            Push(ev);
        }
    }

    struct EventSlot {
        std::atomic<u32> state{}; ///< 0 free, 1 being written, 2 ready
        Event ev;
    };

    void Push(const Event& ev) {
        const unsigned kind = static_cast<unsigned>(ev.kind);
        const u64 n = kind_seen_[kind].fetch_add(1, std::memory_order_relaxed);
        const bool sampled = n < kLogFirst || (n - kLogFirst) % kLogEvery == 0;
        if (!sampled || Get(counters.log_lines) >= kLogMax) {
            return;
        }
        PushForced(ev);
    }

    void PushForced(const Event& ev) {
        const u64 index = head_.fetch_add(1, std::memory_order_relaxed);
        EventSlot& slot = events_[index % kEventSlots];
        u32 expected = 0;
        if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
            Inc(counters.log_dropped);
            return;
        }
        slot.ev = ev;
        slot.state.store(2, std::memory_order_release);
        Inc(counters.log_lines);
    }

    std::array<EventSlot, kEventSlots> events_{};
    std::atomic<u64> head_{0};
    u64 tail_ = 0;
    std::atomic<bool> drain_busy_{false};
    std::atomic<u64> last_drain_{0};
    std::array<Counter, kEventKinds> kind_seen_{};
    std::atomic<size_t> last_thread_{0};
    std::atomic<bool> p0_claimed_{false};
    std::atomic<bool> p0_violated_{false};
    std::vector<u64> at_violation_;
    std::vector<u64> last_report_;
    static inline thread_local const Batch* current_batch_ = nullptr;
};

} // namespace VideoCommon::ListDiag
