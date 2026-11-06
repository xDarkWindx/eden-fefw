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
#include "video_core/list_diag/thread_mark.h"

namespace VideoCommon::ListDiag {

struct Range {
    u64 addr = 0;
    u64 size = 0;
    u32 mask = 0; ///< Cores whose lot contributed to this range
};

enum class Sample : u8 { None, Equal, Different, Unverified };

enum class EventKind : u8 { PopHit, PopApplied, PopNever, Blk, P0Violated, Blkcmp };
constexpr unsigned kEventKinds = 6;

/// blkcmp: the at_risk parts of one request (3.8.1); more than this and the range is discarded, not truncated.
constexpr unsigned kBlkcmpParts = 16;
constexpr u64 kBlkcmpAlign = 64;

enum class BlkcmpReason : u8 { NoUpload, NoMemoryMaps, NoSingleBuffer, Stale };

struct Event {
    EventKind kind = EventKind::PopHit;
    Sample sample = Sample::None;
    bool ambiguous = false;
    bool incomplete = false; ///< blk: coverage declared incomplete (3.7.3)
    u32 core = 0;
    u64 lot = 0;
    u64 notice_addr = 0;
    u32 notice_size = 0;
    std::array<u64, 11> v{};
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
        bool sampled = true; ///< false: the page is outside the sample and nothing was recorded
        bool registered = false;
        Handle handle;
    };
    /// The writer-side counters of all cores, summed (3.7.5). Not a consistent cut while the game runs.
    struct NoticeSums {
        u64 seen = 0, sampled = 0, registered = 0, ambiguous = 0, late = 0, overflow = 0, expired = 0,
            floor_raises = 0;
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
    std::array<WriterCounters, N> writer_counters{};
    std::array<WriterShared, N> writer_shared{};
    bool pop_on = true;
    bool blk_on = true;
    /// The sample (3.7.1): one page of every `sample_n` (1 = all). Fixed for the session; the
    /// emulator sets them from EDEN_LIST_DIAG_SAMPLE and _SEED before the first notice.
    u64 sample_n = 1;
    u64 sample_seed = 1;
    /// False when EDEN_HELD_WRITE_MERGE=0: the final ranges of a lot may overlap, premise pi2 of
    /// 3.7.3 fails, and every range is declared incomplete (blk_incomplete_nomerge).
    bool merge_on = true;

    // ---------------------------------------------------------------------------------------
    // T3a.2 blkcmp (docs/list-diag-design.md, 3.8): configuration, fixed for the session.
    bool blkcmp_on = false;        ///< requires blk
    u64 blkcmp_seed = 0;
    u64 blkcmp_odds = 4;           ///< a range is drawn with probability 1/odds
    u64 blkcmp_rate = 5;           ///< sustained comparisons per second
    u64 blkcmp_burst = 5;          ///< bucket capacity
    u64 blkcmp_max_bytes = 4096;   ///< cap of at_risk bytes per range

    struct BlkcmpPart {
        u64 addr = 0;
        u64 size = 0;
    };
    /// What the decision (AccountBlock, an affirmed range with at_risk > 0, drawn) hands to the Inline
    /// of the same range: the bytes to read from the buffer. Thread-local, like current_batch_.
    struct BlkcmpRequest {
        bool valid = false;
        u64 addr = 0;
        u64 size = 0;
        u32 mask = 0;
        std::array<u64, N> lots{};
        std::array<BlkcmpPart, kBlkcmpParts> parts{};
        unsigned count = 0;
        u64 total = 0; ///< at_risk bytes
    };

    static constexpr u64 AlignUp(u64 value) {
        return (value + kBlkcmpAlign - 1) / kBlkcmpAlign * kBlkcmpAlign;
    }
    /// Offsets of each part in the staging buffer, each aligned to 64 B; returns the size to request.
    static u64 BlkcmpLayout(const BlkcmpRequest& request, std::array<u64, kBlkcmpParts>& offsets) {
        u64 cursor = 0;
        for (unsigned i = 0; i < request.count; ++i) {
            offsets[i] = cursor;
            cursor += AlignUp(request.parts[i].size);
        }
        return cursor;
    }

    /// The request of this thread if it is for [addr, addr + size); a request for another range is stale
    /// (counted, dropped). Null when there is none.
    const BlkcmpRequest* BlkcmpPending(u64 addr, u64 size) {
        BlkcmpRequest& request = blkcmp_request_;
        if (!request.valid) {
            return nullptr;
        }
        if (request.addr != addr || request.size != size) {
            Inc(counters.blkcmp_discard_stale);
            request.valid = false;
            return nullptr;
        }
        return &request;
    }

    /// The pending request ends without a comparison.
    void BlkcmpDiscard(BlkcmpReason reason) {
        BlkcmpRequest& request = blkcmp_request_;
        if (!request.valid) {
            return;
        }
        switch (reason) {
        case BlkcmpReason::NoUpload: Inc(counters.blkcmp_discard_no_upload); break;
        case BlkcmpReason::NoMemoryMaps: Inc(counters.blkcmp_discard_no_memory_maps); break;
        case BlkcmpReason::NoSingleBuffer: Inc(counters.blkcmp_discard_no_single_buffer); break;
        case BlkcmpReason::Stale: Inc(counters.blkcmp_discard_stale); break;
        }
        request.valid = false;
    }

    /// Takes a token of the bucket (sustained `blkcmp_rate` per second, bursts up to `blkcmp_burst`).
    /// None: the request is discarded as budget. The token is taken at the consumption, not at the draw.
    bool BlkcmpTakeToken() {
        BlkcmpRequest& request = blkcmp_request_;
        const u64 now = Hooks::NowUs();
        constexpr u64 kToken = 1'000'000; // micro-tokens
        if (!bucket_started_) {
            bucket_started_ = true;
            bucket_tokens_ = blkcmp_burst * kToken;
            bucket_last_us_ = now;
        } else if (now > bucket_last_us_) {
            // rate tokens per second = rate micro-tokens per microsecond
            bucket_tokens_ = std::min<u64>(blkcmp_burst * kToken, bucket_tokens_ + (now - bucket_last_us_) * blkcmp_rate);
            bucket_last_us_ = now;
        }
        if (bucket_tokens_ < kToken) {
            Inc(counters.blkcmp_discard_budget);
            request.valid = false;
            return false;
        }
        bucket_tokens_ -= kToken;
        return true;
    }

    /// Compares the old bytes of the GPU (`old_bytes`, the parts one after the other at `offsets`) with the payload
    /// the upload sends (`payload` covers [request.addr, request.addr + request.size)). Ends the request.
    /// Returns whether any byte differs. `finish_us` is the time of the Finish. The guest is not read.
    bool BlkcmpCompare(const u8* old_bytes, const std::array<u64, kBlkcmpParts>& offsets,
                       std::span<const u8> payload, u64 finish_us, bool staging_coherent) {
        BlkcmpRequest& request = blkcmp_request_;
        if (!request.valid || payload.size() < request.size) {
            BlkcmpDiscard(BlkcmpReason::Stale);
            return false;
        }
        u64 compared = 0, different = 0;
        bool have_first = false;
        u64 first_addr = 0, first_old = 0, first_new = 0;
        for (unsigned i = 0; i < request.count; ++i) {
            const BlkcmpPart& part = request.parts[i];
            const u8* gpu = old_bytes + offsets[i];
            const u8* upload = payload.data() + (part.addr - request.addr);
            compared += part.size;
            for (u64 b = 0; b < part.size; ++b) {
                if (gpu[b] != upload[b]) {
                    ++different;
                    if (!have_first) {
                        have_first = true;
                        first_addr = part.addr + b;
                        const u64 left = std::min<u64>(8, part.size - b);
                        for (u64 k = 0; k < left; ++k) {
                            first_old |= u64{gpu[b + k]} << (8 * k);
                            first_new |= u64{upload[b + k]} << (8 * k);
                        }
                    }
                }
            }
        }
        Inc(counters.blkcmp_compared);
        switch (t_role) {
        case ThreadRole::Back: Inc(counters.blkcmp_compared_back); break;
        case ThreadRole::Front: Inc(counters.blkcmp_compared_front); break;
        default: Inc(counters.blkcmp_compared_other); break;
        }
        Inc(counters.blkcmp_bytes_compared, compared);
        Inc(counters.blkcmp_bytes_different, different);
        // Was the staging memory coherent, or did the caller have to invalidate it before this read (3.8.10)?
        Inc(staging_coherent ? counters.blkcmp_staging_coherent : counters.blkcmp_staging_invalidated);
        Inc(counters.blkcmp_finish_n);
        Inc(counters.blkcmp_finish_us_sum, finish_us);
        Max(counters.blkcmp_finish_us_max, finish_us);
        if (different == 0) {
            Inc(counters.blkcmp_equal);
        } else {
            Inc(counters.blkcmp_different);
            Event ev;
            ev.kind = EventKind::Blkcmp;
            ev.core = request.mask;
            for (unsigned c = 0; c < N; ++c) {
                if (((request.mask >> c) & 1) != 0) {
                    ev.lot = request.lots[c];
                    break;
                }
            }
            ev.v[0] = request.addr;
            ev.v[1] = request.size;
            ev.v[2] = request.count;
            ev.v[3] = compared;
            ev.v[4] = different;
            ev.v[5] = first_addr;
            ev.v[6] = first_old;
            ev.v[7] = first_new;
            ev.v[8] = static_cast<u64>(t_role);
            ev.v[9] = finish_us;
            ev.v[10] = staging_coherent ? 1 : 0;
            Push(ev);
        }
        request.valid = false;
        return different != 0;
    }

    /// The end of CachedWriteMemory: a request that is still pending never reached the upload.
    void BlkcmpExpire() {
        BlkcmpDiscard(BlkcmpReason::NoUpload);
    }

    static constexpr u64 SplitMix64(u64 x) {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }
    /// The one predicate of the sample, over the 4 KiB page of the address and never over the core.
    [[nodiscard]] bool Sampled(u64 addr) const {
        if (sample_n <= 1) {
            return true;
        }
        const u64 h = SplitMix64(sample_seed ^ (addr >> 12));
        return (sample_n & (sample_n - 1)) == 0 ? (h & (sample_n - 1)) == 0 : h % sample_n == 0;
    }

    struct Value {
        const char* name;
        Group group;
        u64 value;
    };
    /// Every value the report and the violation snapshot walk, in a fixed order: the shared counters,
    /// then the sums of the writer's per-core counters (3.7.5).
    [[nodiscard]] std::vector<Value> Snapshot() {
        std::vector<Value> out;
        for (const FieldRef& f : Fields(counters)) {
            out.push_back({f.name, f.group, Get(*f.counter)});
        }
        const NoticeSums w = Sums();
        out.push_back({"notices_seen", Group::Sample, w.seen});
        out.push_back({"notices_sampled", Group::Sample, w.sampled});
        out.push_back({"notice_registered", Group::Notices, w.registered});
        out.push_back({"notice_ambiguous", Group::Notices, w.ambiguous});
        out.push_back({"notice_late", Group::Notices, w.late});
        out.push_back({"notice_expired", Group::Notices, w.expired});
        out.push_back({"notice_overflow", Group::Notices, w.overflow});
        out.push_back({"floor_raises", Group::Notices, w.floor_raises});
        return out;
    }

    [[nodiscard]] NoticeSums Sums() const {
        NoticeSums sums;
        for (const WriterCounters& w : writer_counters) {
            sums.seen += w.seen.load(std::memory_order_relaxed);
            sums.sampled += w.sampled.load(std::memory_order_relaxed);
            sums.registered += w.registered.load(std::memory_order_relaxed);
            sums.ambiguous += w.ambiguous.load(std::memory_order_relaxed);
            sums.late += w.late.load(std::memory_order_relaxed);
            sums.overflow += w.overflow.load(std::memory_order_relaxed);
            sums.expired += w.expired.load(std::memory_order_relaxed);
            sums.floor_raises += w.floor_raises.load(std::memory_order_relaxed);
        }
        return sums;
    }

    // ---------------------------------------------------------------------------------------
    // core/memory.cpp:765: the Collect, observed.

    /// Reads (done, started) before and started after the real Collect: the notice belongs to lot
    /// done + 1 of this core only if no Gather was running before and none started meanwhile
    /// (docs/list-diag-design.md, 3.3). Registered AFTER the Collect and BEFORE the caller's
    /// memcpy: it is a registered notice, never a completed write (3.5).
    template <class Manager>
    Claim CollectObserved(Manager& manager, unsigned core, u64 addr, u32 size) {
        WriterCounters& wc = writer_counters[core];
        WriterShared& ws = writer_shared[core];
        wc.Bump(wc.seen);
        if (!Sampled(addr)) {
            // Outside the sample: the Collect of the diagnostic (seq_cst on `current`, which the proof of
            // 3.3 needs for every access) and nothing else: no ledger, no lot loads, nothing shared.
            manager.template CollectT<true, Hooks>(addr, size);
            Claim out;
            out.sampled = false;
            return out;
        }
        wc.Bump(wc.sampled);
        // O1: from here until O2 a notice of this core may be between the Collect and its record.
        const bool use_open = !NoOpenFlag();
        if (use_open) {
            ws.open.store(1, std::memory_order_seq_cst);
        }
        struct CloseOpen {
            WriterShared& ws;
            bool use;
            ~CloseOpen() {
                Hooks::At("w.before_open_clear");
                if (use) {
                    ws.open.store(0, std::memory_order_release); // O2: release, as designed
                }
            }
        } close_open{ws, use_open};
        Hooks::At("w.after_open");
        auto& st = lots[core];
        const u64 d = st.lot_done.load(std::memory_order_seq_cst);
        const u64 s = st.lot_started.load(std::memory_order_seq_cst);
        Hooks::At("w.after_r0");
        // With the D1 split (2 KiB) this is one or two pieces, all between the reads s and s2.
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
        // Every lot this notice can belong to is <= hi (3.7.3, (b) and (c)).
        const u64 hi = claim.ambiguous ? s2 + 1 : d + 1;
        Hooks::At("w.after_s2");
        const u64 now = Hooks::NowUs(); // the one clock read of the notice
        Reclaimed reclaimed;
        claim.handle = ledger.Register(core, addr, size, claim.ambiguous ? hi : claim.lot, claim.ambiguous, now,
                                       &reclaimed, wc, ws);
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
            // The notice reached no record: the coverage of every lot it can belong to is incomplete.
            if (!NoCoverage()) {
                RaiseIncomplete(ws, wc, hi);
            }
            Hooks::At("w.after_overflow");
            return claim;
        }
        wc.Bump(wc.registered);
        if (claim.ambiguous) {
            wc.Bump(wc.ambiguous);
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
        if (claim.lot != 0 && applying >= claim.lot) {
            const MarkResult r = ledger.Mark(*claim.handle.slot, claim.handle.tag(), MarkKind::Late, now);
            if (r == MarkResult::Ok) {
                wc.Bump(wc.late);
                // The applier skips a late record: this lot's coverage is incomplete (before the Free).
                if (!NoCoverage()) {
                    RaiseIncomplete(ws, wc, claim.lot);
                }
                ledger.Free(core, *claim.handle.slot, claim.handle.gen);
            }
        } else {
            ledger.Mark(*claim.handle.slot, claim.handle.tag(), MarkKind::Checked, now);
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
            // Only the ranges of the sample use their cores (NoteApply returns early for the others).
            if (Sampled(address)) {
                for (const Range& r : gathered.pre) {
                    if (r.addr >= address && r.addr + r.size <= address + size) {
                        merged.mask |= r.mask;
                    }
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
        Inc(counters.apply_ranges_seen);
        if (!Sampled(addr)) {
            return; // outside the sample: no sweep, no blk (3.7.2)
        }
        Inc(counters.apply_ranges_selected);
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
        // rho: BEFORE the sweep, whether a sampled notice of a core of this range is being registered.
        std::array<u64, N> open_before{};
        for (unsigned c = 0; c < N; ++c) {
            if (((mask >> c) & 1) != 0) {
                open_before[c] = writer_shared[c].open.load(std::memory_order_seq_cst);
            }
        }
        Hooks::At("a.after_open_read");
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
        Hooks::At("a.after_sweep");
        // phi: AFTER the sweep, the coverage floor of each core against the lot taken for it.
        Cover cover;
        cover.mask = mask;
        cover.lots = batch != nullptr ? batch->lots.data() : nullptr;
        for (unsigned c = 0; c < N; ++c) {
            if (((mask >> c) & 1) == 0) {
                continue;
            }
            if (open_before[c] != 0) {
                cover.open = true;
            }
            const u64 floor = writer_shared[c].incomplete_floor.load(std::memory_order_seq_cst);
            if (batch != nullptr && floor >= batch->lots[c]) {
                cover.floor = true;
            }
        }
        cover.nomerge = !merge_on;
        if (blk_on) {
            AccountBlock(addr, size, exact, ambiguous || !linked, cover, gpu_modified, cpu_read, track_cpu_reads);
        }
    }

    // ---------------------------------------------------------------------------------------
    // buffer_cache.h: the loop of PopAsyncBuffers.

    /// `bytes` are the download bytes the pop is about to write over [start, end).
    void NotePop(u64 start, u64 end, const u8* bytes) {
        Inc(counters.pop_ranges);
        // 3.7.4: the range is cut by page only to COUNT and to skip the sweep when no fragment is in
        // the sample. The write of the download is unchanged and the sweep is one over the whole
        // range: a record lives in one page, so it falls in exactly one fragment.
        const u64 first_page = start >> 12;
        const u64 last_page = (end - 1) >> 12;
        const u64 fragments = last_page - first_page + 1;
        u64 selected = fragments;
        if (sample_n > 1) {
            selected = 0;
            for (u64 page = first_page; page <= last_page; ++page) {
                selected += Sampled(page << 12) ? 1 : 0;
            }
        }
        Inc(counters.pop_fragments, fragments);
        Inc(counters.pop_fragments_selected, selected);
        if (selected == 0) {
            return;
        }
        std::vector<u64> hit_pages;
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
                hit_pages.push_back(lo >> 12);
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
                hit_pages.push_back(lo >> 12);
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
                ev.v[4] = fragments;
                ev.v[5] = selected;
                Push(ev);
            }
        });
        if (any) {
            // One per ORIGINAL pop; the fragments hit are counted apart (the per-page rate is the
            // unbiased one).
            Inc(counters.pop_over_notice);
            Inc(counters.pop_over_notice_bytes, flagged_bytes);
            std::sort(hit_pages.begin(), hit_pages.end());
            Inc(counters.pop_over_notice_fragments,
                static_cast<u64>(std::unique(hit_pages.begin(), hit_pages.end()) - hit_pages.begin()));
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
        at_violation_.clear();
        for (const Value& v : Snapshot()) {
            at_violation_.push_back(v.value);
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
        const std::vector<Value> fields = Snapshot();
        std::vector<u64> now;
        for (const Value& f : fields) {
            now.push_back(f.value);
        }
        if (last_report_.size() != now.size()) {
            last_report_.assign(now.size(), 0);
        }
        std::vector<std::string> lines;
        // The sums of the cores are read while the game runs: not a consistent cut (3.7.5).
        const char* prefix = P0Violated() ? "[ListDiag][P0 VIOLATED: results after this are not reliable] (aprox.) "
                                          : "[ListDiag] (aprox.) ";
        const Group groups[] = {Group::Notices, Group::Sample, Group::Pop,   Group::Apply,
                                Group::Blk,     Group::Blkcmp, Group::Calls, Group::Log};
        const char* names[] = {"notices", "sample", "pop", "apply", "blk", "blkcmp", "calls", "log"};
        for (size_t g = 0; g < 8; ++g) {
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
            if (groups[g] == Group::Calls) {
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
    std::vector<std::string> SummaryLines(const char* tag = "summary") {
        CheckViolation();
        const std::vector<Value> fields = Snapshot();
        std::vector<std::string> lines;
        const bool is_final = std::string{tag} == "summary";
        // The 30 s totals are read with the game running: approximate. Only the final summary, with
        // the execution stopped, can be conserved (3.7.5).
        const std::string approx = is_final ? "" : " (aprox.)";
        if (P0Violated()) {
            lines.push_back(std::string{"[ListDiag]["} + tag + "] *** precondicao P0 violada: os resultados do diagnostico a partir da primeira "
                            "violacao nao sao confiaveis (gather_overlapped=" +
                            std::to_string(Get(counters.gather_overlapped)) +
                            ", apply_out_of_order=" + std::to_string(Get(counters.apply_out_of_order)) + ") ***");
        }
        std::string line = std::string{"[ListDiag]["} + tag + "]" + approx;
        for (const Value& f : fields) {
            if (f.value != 0) {
                line += std::string{" "} + f.name + "=" + std::to_string(f.value);
            }
        }
        lines.push_back(line);
        const NoticeSums w = Sums();
        {
            char b[320];
            std::snprintf(b, sizeof(b),
                          "[ListDiag][%s]%s amostra: N=%llu semente=0x%llx notices_seen=%llu notices_sampled=%llu "
                          "sampled/seen=%.4f (fracao de AVISOS selecionados, 1/N=%.4f; nada e multiplicado por N)",
                          tag, approx.c_str(), static_cast<unsigned long long>(sample_n),
                          static_cast<unsigned long long>(sample_seed), static_cast<unsigned long long>(w.seen),
                          static_cast<unsigned long long>(w.sampled),
                          w.seen != 0 ? static_cast<double>(w.sampled) / static_cast<double>(w.seen) : 0.0,
                          1.0 / static_cast<double>(sample_n));
            lines.push_back(b);
            std::snprintf(b, sizeof(b),
                          "[ListDiag][%s]%s cobertura: notice_overflow=%llu notice_expired=%llu notice_late=%llu "
                          "notice_ambiguous=%llu floor_raises=%llu blk_incomplete=%llu (open=%llu floor=%llu nomerge=%llu) "
                          "blk_affirmed=%llu; pop_over_notice nas paginas amostradas e um piso enquanto overflow e expired "
                          "forem diferentes de zero",
                          tag, approx.c_str(), static_cast<unsigned long long>(w.overflow),
                          static_cast<unsigned long long>(w.expired), static_cast<unsigned long long>(w.late),
                          static_cast<unsigned long long>(w.ambiguous), static_cast<unsigned long long>(w.floor_raises),
                          static_cast<unsigned long long>(Get(counters.blk_incomplete)),
                          static_cast<unsigned long long>(Get(counters.blk_incomplete_open)),
                          static_cast<unsigned long long>(Get(counters.blk_incomplete_floor)),
                          static_cast<unsigned long long>(Get(counters.blk_incomplete_nomerge)),
                          static_cast<unsigned long long>(Get(counters.blk_affirmed)));
            lines.push_back(b);
        }
        if (blkcmp_on) {
            char b[900];
            const auto n = [&](const Counter& c) { return static_cast<unsigned long long>(Get(c)); };
            std::snprintf(b, sizeof(b),
                          "[ListDiag][%s]%s blkcmp: bytes antigos da GPU diferentes do payload que os substituiu: %llu de %llu "
                          "bytes comparados (%llu de %llu faixas); sorteadas %llu de %llu elegiveis (as afirmadas com at_risk>0, "
                          "1/%llu, semente=0x%llx; NAO sao amostra aleatoria de todas as aplicacoes); descartadas: faixa=%llu "
                          "tamanho=%llu sem_upload=%llu sem_mapas=%llu sem_buffer_unico=%llu orcamento=%llu obsoleto=%llu; "
                          "balde %llu/s rajada %llu, teto %llu B; por thread: back=%llu front=%llu other/unknown=%llu; "
                          "Finish n=%llu soma=%lluus max=%lluus (perturbacao: um Finish sob o buffer_cache.mutex por comparacao); "
                          "staging: coerente em %llu de %llu comparacoes, invalidado em %llu (coerente numa rodada nova vale para as "
                          "alocacoes dela, nao prova o tipo das antigas). "
                          "Isso nao prova corrupcao visivel nem exclui uma escrita legitima concorrente.",
                          tag, approx.c_str(), n(counters.blkcmp_bytes_different), n(counters.blkcmp_bytes_compared),
                          n(counters.blkcmp_different), n(counters.blkcmp_compared), n(counters.blkcmp_drawn),
                          n(counters.blkcmp_eligible), static_cast<unsigned long long>(blkcmp_odds),
                          static_cast<unsigned long long>(blkcmp_seed), n(counters.blkcmp_discard_ranges),
                          n(counters.blkcmp_discard_size), n(counters.blkcmp_discard_no_upload),
                          n(counters.blkcmp_discard_no_memory_maps), n(counters.blkcmp_discard_no_single_buffer),
                          n(counters.blkcmp_discard_budget), n(counters.blkcmp_discard_stale),
                          static_cast<unsigned long long>(blkcmp_rate), static_cast<unsigned long long>(blkcmp_burst),
                          static_cast<unsigned long long>(blkcmp_max_bytes), n(counters.blkcmp_compared_back),
                          n(counters.blkcmp_compared_front), n(counters.blkcmp_compared_other), n(counters.blkcmp_finish_n),
                          n(counters.blkcmp_finish_us_sum), n(counters.blkcmp_finish_us_max),
                          n(counters.blkcmp_staging_coherent), n(counters.blkcmp_compared),
                          n(counters.blkcmp_staging_invalidated));
            lines.push_back(b);
        }
        if (is_final) {
            // Conservation: valid only because the execution is stopped. Both sides and the residual.
            const auto identity = [&](const char* name, u64 left, u64 right) {
                char b[256];
                std::snprintf(b, sizeof(b), "[ListDiag][%s] conservacao (so com a execucao parada) %s: %llu vs %llu (residuo %lld)",
                              tag, name, static_cast<unsigned long long>(left), static_cast<unsigned long long>(right),
                              static_cast<long long>(left) - static_cast<long long>(right));
                lines.push_back(b);
            };
            identity("sampled = registered + overflow", w.sampled, w.registered + w.overflow);
            identity("registered = applied + late + expired + pendentes", w.registered,
                     Get(counters.notice_applied) + w.late + w.expired + PublishedCount());
            if (blk_on) {
                identity("blk_applies = apply_ranges_selected", Get(counters.blk_applies),
                         Get(counters.apply_ranges_selected));
                identity("blk_applies_over_gpu = ambiguous + incomplete + affirmed", Get(counters.blk_applies_over_gpu),
                         Get(counters.blk_ambiguous) + Get(counters.blk_incomplete) + Get(counters.blk_affirmed));
            }
            if (blkcmp_on) {
                identity("blkcmp_eligible >= blkcmp_drawn (0 = ok)",
                         Get(counters.blkcmp_eligible) >= Get(counters.blkcmp_drawn) ? 0 : 1, 0);
                identity("blkcmp_drawn = compared + descartes", Get(counters.blkcmp_drawn),
                         Get(counters.blkcmp_compared) + Get(counters.blkcmp_discard_ranges) +
                             Get(counters.blkcmp_discard_size) + Get(counters.blkcmp_discard_no_upload) +
                             Get(counters.blkcmp_discard_no_memory_maps) +
                             Get(counters.blkcmp_discard_no_single_buffer) + Get(counters.blkcmp_discard_budget) +
                             Get(counters.blkcmp_discard_stale));
                identity("blkcmp_compared = equal + different", Get(counters.blkcmp_compared),
                         Get(counters.blkcmp_equal) + Get(counters.blkcmp_different));
                identity("blkcmp_compared = staging_coherent + staging_invalidated", Get(counters.blkcmp_compared),
                         Get(counters.blkcmp_staging_coherent) + Get(counters.blkcmp_staging_invalidated));
            }
            if (pop_on) {
                char b[200];
                std::snprintf(b, sizeof(b),
                              "[ListDiag][%s] conservacao pop: pop_ranges=%llu <= pop_fragments=%llu; "
                              "pop_fragments_selected=%llu <= pop_fragments (%s)",
                              tag, static_cast<unsigned long long>(Get(counters.pop_ranges)),
                              static_cast<unsigned long long>(Get(counters.pop_fragments)),
                              static_cast<unsigned long long>(Get(counters.pop_fragments_selected)),
                              Get(counters.pop_ranges) <= Get(counters.pop_fragments) &&
                                      Get(counters.pop_fragments_selected) <= Get(counters.pop_fragments)
                                  ? "ok"
                                  : "VIOLADA");
                lines.push_back(b);
            }
            lines.push_back(std::string{"[ListDiag]["} + tag +
                            "] escala: nenhuma (valores observados; a amostra e 1/N das paginas e nao e multiplicada)");
        }
        for (unsigned core = 0; core < N; ++core) {
            const u64 calls = Get(counters.gather_calls[core]);
            if (calls != 0) {
                lines.push_back(std::string{"[ListDiag]["} + tag + "] gather[" + std::to_string(core) + "]=" + std::to_string(calls) +
                                " empty=" + std::to_string(Get(counters.gather_empty[core])));
            }
        }
        if (P0Violated()) {
            std::string before = std::string{"[ListDiag]["} + tag + "] counters at the first violation:";
            for (size_t i = 0; i < fields.size() && i < at_violation_.size(); ++i) {
                if (at_violation_[i] != 0) {
                    before += std::string{" "} + fields[i].name + "=" + std::to_string(at_violation_[i]);
                }
            }
            lines.push_back(before);
        }
        return lines;
    }

    /// Published slots (records not yet finished or freed), counted from the slots themselves.
    [[nodiscard]] u64 PublishedCount() {
        u64 n = 0;
        for (unsigned core = 0; core < ledger.Cores(); ++core) {
            for (unsigned i = 0; i < ledger.SlotsPerCore(); ++i) {
                n += (ledger.SlotAt(core, i).state.load(std::memory_order_seq_cst) & 3) == kPublished ? 1 : 0;
            }
        }
        return n;
    }

    std::string FormatEvent(const Event& ev) const {
        char b[512];
        const auto hex = [](u64 x) { return static_cast<unsigned long long>(x); };
        switch (ev.kind) {
        case EventKind::PopHit:
            std::snprintf(b, sizeof(b),
                          "[ListDiag][pop] hit: notice=[0x%llx,+%u) core=%u lot=%llu download=[0x%llx,0x%llx) overlap=%llu "
                          "order=notice -> pop@+%lluus (apply pending) pop_fragments=%llu selected=%llu",
                          hex(ev.notice_addr), ev.notice_size, ev.core, hex(ev.lot), hex(ev.v[0]), hex(ev.v[1]),
                          hex(ev.v[2]), hex(ev.v[3]), hex(ev.v[4]), hex(ev.v[5]));
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
            } else if (ev.incomplete) {
                std::snprintf(at_risk, sizeof(at_risk), "incomplete(would_be=%llu)", hex(ev.v[3]));
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
        case EventKind::Blkcmp: {
            const char* thread = ev.v[8] == static_cast<u64>(ThreadRole::Back)    ? "back"
                                 : ev.v[8] == static_cast<u64>(ThreadRole::Front) ? "front"
                                                                                  : "other/unknown";
            std::snprintf(b, sizeof(b),
                          "[ListDiag][blkcmp] different: range=[0x%llx,+%llu) cores=0x%x lot=%llu parts=%llu bytes=%llu "
                          "diff_bytes=%llu first=0x%llx gpu_old=0x%016llx payload=0x%016llx (8 bytes, little endian) "
                          "thread=%s finish=%lluus staging=%s",
                          hex(ev.v[0]), hex(ev.v[1]), ev.core, hex(ev.lot), hex(ev.v[2]), hex(ev.v[3]), hex(ev.v[4]),
                          hex(ev.v[5]), hex(ev.v[6]), hex(ev.v[7]), thread, hex(ev.v[9]),
                          ev.v[10] != 0 ? "coherent" : "invalidated");
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
    struct Cover {
        bool open = false;
        bool floor = false;
        bool nomerge = false;
        u32 mask = 0;                  ///< Cores of the range (test hook only)
        const u64* lots = nullptr;     ///< Lots taken for them (test hook only)
        [[nodiscard]] bool Incomplete() const {
            return open || floor || nomerge;
        }
    };

    /// An affirmed range with at_risk > 0 (3.8.1): eligible; drawn with probability 1/odds from the own seed and the
    /// number of order of the eligible (the applications are serialised, so the sequence is one); then the cap and
    /// the parts. What survives becomes the request of this thread.
    void BlkcmpOffer(u64 addr, u64 size, const Cover& cover, const std::array<BlkcmpPart, kBlkcmpParts>& parts,
                     unsigned count, bool overflow, u64 at_risk) {
        Inc(counters.blkcmp_eligible);
        const u64 k = blkcmp_k_++;
        if (blkcmp_odds > 1 && SplitMix64(blkcmp_seed ^ k) % blkcmp_odds != 0) {
            return;
        }
        Inc(counters.blkcmp_drawn);
        if (overflow) {
            Inc(counters.blkcmp_discard_ranges);
            return;
        }
        if (at_risk > blkcmp_max_bytes) {
            Inc(counters.blkcmp_discard_size);
            return;
        }
        BlkcmpRequest& request = blkcmp_request_;
        if (request.valid) {
            Inc(counters.blkcmp_discard_stale); // an older request that nobody ended
        }
        request.valid = true;
        request.addr = addr;
        request.size = size;
        request.mask = cover.mask;
        for (unsigned c = 0; c < N; ++c) {
            request.lots[c] = cover.lots != nullptr ? cover.lots[c] : 0;
        }
        request.parts = parts;
        request.count = count;
        request.total = at_risk;
    }

    u64 blkcmp_k_ = 0;
    bool bucket_started_ = false;
    u64 bucket_tokens_ = 0;
    u64 bucket_last_us_ = 0;
    static inline thread_local BlkcmpRequest blkcmp_request_{};

    /// Test-only mutants (the tests prove they can see the failure): coverage never declared, and the
    /// `open` flag never set. Constant false in the emulator.
    static bool NoCoverage() {
        if constexpr (Hooks::kTest) {
            return Hooks::NaiveNoCoverage();
        }
        return false;
    }
    static bool NoOpenFlag() {
        if constexpr (Hooks::kTest) {
            return Hooks::NaiveNoOpenFlag();
        }
        return false;
    }

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
                      const Cover& cover, const RangeSet& gpu_modified, const RangeSet& cpu_read, bool track_cpu_reads) {
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
            if (!ambiguous) {
                Inc(counters.blk_ambiguous_no_record);
            }
            ev.ambiguous = true;
            Push(ev);
            return;
        }
        // at_risk = the GPU bytes of the range that the CPU did not write.
        u64 at_risk = 0, cpu_read_bytes = 0;
        std::array<BlkcmpPart, kBlkcmpParts> risk_parts{};
        unsigned risk_count = 0;
        bool risk_overflow = false;
        for (const auto& [gs, ge] : gpu) {
            u64 cursor = gs;
            const auto account = [&](u64 s, u64 e) {
                if (e <= s) {
                    return;
                }
                at_risk += e - s;
                if (blkcmp_on) {
                    if (risk_count < kBlkcmpParts) {
                        risk_parts[risk_count++] = BlkcmpPart{s, e - s};
                    } else {
                        risk_overflow = true;
                    }
                }
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
        if (cover.Incomplete()) {
            // Coverage is not known to be complete: what T3a would have claimed is NOT claimed (3.7.3).
            Inc(counters.blk_incomplete);
            Inc(cover.open ? counters.blk_incomplete_open
                : cover.floor ? counters.blk_incomplete_floor
                              : counters.blk_incomplete_nomerge);
            Inc(counters.blk_incomplete_gpu_bytes, at_risk);
            ev.incomplete = true;
            ev.v[3] = at_risk;
            if (at_risk != 0) {
                Push(ev);
            }
            return;
        }
        Inc(counters.blk_affirmed);
        if constexpr (Hooks::kTest) {
            Hooks::OnAffirmed(addr, size, cover.mask, cover.lots, merged_exact);
        }
        if (blkcmp_on && at_risk > 0) {
            BlkcmpOffer(addr, size, cover, risk_parts, risk_count, risk_overflow, at_risk);
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
