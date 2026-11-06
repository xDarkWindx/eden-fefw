// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Aggregated counters of the EDEN_LIST_DIAG diagnostic (docs/list-diag-design.md, 4 and 5).
// Header-only and self-contained: the emulator and tools/list-diag-harness include the same file.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ListDiag {

constexpr unsigned kMaxCores = 8; ///< Upper bound of Core::Hardware::NUM_CPU_CORES

using Counter = std::atomic<u64>;

inline void Inc(Counter& counter, u64 amount = 1) {
    counter.fetch_add(amount, std::memory_order_relaxed);
}
inline u64 Get(const Counter& counter) {
    return counter.load(std::memory_order_relaxed);
}
inline void Max(Counter& counter, u64 value) {
    u64 old = counter.load(std::memory_order_relaxed);
    while (old < value && !counter.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
}

/// What a writer moves, one per core and one cache line each (docs/list-diag-design.md, 3.7.5). One
/// writer per core (precondition pi4: the sys_core_guard serialises the system core), so the
/// increment is a relaxed load and store, not a read-modify-write. If pi4 failed a count could be
/// lost, never corrupted, and the conservation of the final summary would show it. The report sums
/// the cores; while the game runs the sum is not a consistent cut.
struct alignas(64) WriterCounters {
    std::atomic<u64> seen{};         ///< Every notice of the core while the diagnostic is on
    std::atomic<u64> sampled{};      ///< ... of a page in the sample
    std::atomic<u64> registered{};   ///< ... that reached a record (ambiguous ones included)
    std::atomic<u64> ambiguous{};
    std::atomic<u64> late{};
    std::atomic<u64> overflow{};     ///< ... that found no free slot
    std::atomic<u64> expired{};      ///< records recycled while not finished
    std::atomic<u64> floor_raises{}; ///< times the coverage floor rose
    void Bump(std::atomic<u64>& field) {
        field.store(field.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};
static_assert(sizeof(WriterCounters) == 64 && alignof(WriterCounters) == 64);

/// What the applier reads about coverage (3.7.3), written only by the writer of the core. Another
/// cache line than WriterCounters: the applier's loads must not invalidate the writer's counters.
struct alignas(64) WriterShared {
    std::atomic<u64> open{};             ///< 1 between the start of a sampled notice's Collect and the end of its registration
    std::atomic<u64> incomplete_floor{}; ///< Every lot of this core up to here has incomplete coverage
};
static_assert(sizeof(WriterShared) == 64 && alignof(WriterShared) == 64);

/// Problem A (the pop window) and problem B (64-byte blocks) have their own counters, as the
/// design requires: fixing one does not fix the other.
struct Counters {
    // The notices (every one ends in exactly one of applied, late, expired, or stays pending). The
    // counters the WRITER moves (registered, late, expired, overflow, ambiguous, and the sample
    // counts) are per core in WriterCounters below, summed at report time; only the applier's is here.
    Counter notice_applied{};
    // Problem A: a pop over a registered notice that was not applied yet.
    Counter pop_ranges{};
    // T3a.1 (docs/list-diag-design.md, 3.7): the sample. Observed values, never scaled by N.
    Counter apply_ranges_seen{};
    Counter apply_ranges_selected{};
    Counter pop_fragments{};
    Counter pop_fragments_selected{};
    Counter pop_over_notice_fragments{};
    Counter pop_over_notice{};
    Counter pop_over_notice_bytes{};
    Counter pop_over_notice_records{};
    Counter pop_over_ambiguous{};
    Counter pop_over_unchecked{};
    Counter window_sample_equal{};
    Counter window_sample_different{};
    Counter window_unverified{};
    Counter window_sample_bytes{};
    Counter notice_to_apply_us_sum{};
    Counter notice_to_apply_us_max{};
    Counter notice_to_apply_n{};
    Counter notice_to_pop_us_sum{};
    Counter notice_to_pop_us_max{};
    Counter notice_to_pop_n{};
    // The rule that retires a notice (lot link, docs/list-diag-design.md, 3.3).
    Counter apply_overlap_other_lot{};
    Counter apply_overlap_ambiguous_lot{};
    Counter apply_partial{};
    Counter apply_unlinked{};
    Counter observation_lost{};
    // Problem B: the application of a notice whose 64-byte block covers GPU-modified bytes.
    Counter blk_applies{};
    Counter blk_applies_over_gpu{};
    Counter blk_bytes_range{};
    Counter blk_bytes_exact{};
    Counter blk_gpu_bytes_covered{};
    Counter blk_gpu_bytes_at_risk{};
    Counter blk_at_risk_cpu_read{};
    Counter blk_at_risk_not_seen_cpu_read{};
    Counter blk_at_risk_unknown{};
    Counter blk_ambiguous{};
    Counter blk_ambiguous_no_record{}; ///< inside blk_ambiguous: the exact list was empty
    Counter blk_incomplete{};          ///< coverage declared incomplete (3.7.3); at_risk not claimed
    Counter blk_incomplete_open{};     ///< ... a sampled notice of a core in the range was being registered
    Counter blk_incomplete_floor{};    ///< ... a notice of the (core, lot) never reached a record
    Counter blk_incomplete_nomerge{};  ///< ... the join of ranges is off: ranges may overlap
    Counter blk_incomplete_gpu_bytes{}; ///< what T3a would have claimed as at_risk in those ranges
    Counter blk_affirmed{};            ///< complete coverage: at_risk is claimed
    // T3a.2 blkcmp (docs/list-diag-design.md, 3.8): the old bytes of the GPU against the upload payload.
    Counter blkcmp_eligible{};         ///< affirmed ranges with at_risk > 0
    Counter blkcmp_drawn{};            ///< ... drawn (1/M)
    Counter blkcmp_discard_ranges{};   ///< ... more at_risk parts than the request holds
    Counter blkcmp_discard_size{};     ///< ... at_risk bytes above the cap
    Counter blkcmp_discard_no_upload{};        ///< CachedWriteMemory returned before the upload
    Counter blkcmp_discard_no_memory_maps{};   ///< the backend has no memory maps (USE_MEMORY_MAPS false)
    Counter blkcmp_discard_no_single_buffer{}; ///< no one buffer holds the range
    Counter blkcmp_discard_budget{};   ///< no token in the bucket
    Counter blkcmp_discard_stale{};    ///< the request was not of this call (should not happen)
    Counter blkcmp_compared{};
    Counter blkcmp_compared_back{};    ///< ... on the thread marked back
    Counter blkcmp_compared_front{};   ///< ... on the thread marked front
    Counter blkcmp_compared_other{};   ///< ... on a thread with no mark (other/unknown)
    Counter blkcmp_equal{};
    Counter blkcmp_different{};
    Counter blkcmp_bytes_compared{};
    Counter blkcmp_bytes_different{};
    Counter blkcmp_staging_coherent{};   ///< the staging memory was host coherent
    Counter blkcmp_staging_invalidated{}; ///< it was not, and was invalidated before the read (3.8.10)
    Counter blkcmp_finish_n{};
    Counter blkcmp_finish_us_sum{};
    Counter blkcmp_finish_us_max{};
    // The calls that apply notices, counted for real (no assumption of one per list or fence).
    Counter invalidate_calls{};
    Counter invalidate_empty{};
    std::array<Counter, kMaxCores> gather_calls{};
    std::array<Counter, kMaxCores> gather_empty{};
    Counter gather_overlapped{};
    Counter apply_out_of_order{};
    Counter invalidate_thread_switches{};
    // The log (limited: the first N of each kind, then a sample).
    Counter log_lines{};
    Counter log_dropped{};
};

enum class Group { Notices, Sample, Pop, Apply, Blk, Blkcmp, Calls, Log };

struct FieldRef {
    const char* name;
    Group group;
    Counter* counter;
};

/// Every counter, in a fixed order (the report and the violation snapshot walk this list).
inline std::vector<FieldRef> Fields(Counters& c) {
    std::vector<FieldRef> f;
#define LD_FIELD(group, name) f.push_back({#name, Group::group, &c.name})
    LD_FIELD(Notices, notice_applied);
    LD_FIELD(Sample, apply_ranges_seen);
    LD_FIELD(Sample, apply_ranges_selected);
    LD_FIELD(Sample, pop_fragments);
    LD_FIELD(Sample, pop_fragments_selected);
    LD_FIELD(Sample, pop_over_notice_fragments);
    LD_FIELD(Pop, pop_ranges);
    LD_FIELD(Pop, pop_over_notice);
    LD_FIELD(Pop, pop_over_notice_bytes);
    LD_FIELD(Pop, pop_over_notice_records);
    LD_FIELD(Pop, pop_over_ambiguous);
    LD_FIELD(Pop, pop_over_unchecked);
    LD_FIELD(Pop, window_sample_equal);
    LD_FIELD(Pop, window_sample_different);
    LD_FIELD(Pop, window_unverified);
    LD_FIELD(Pop, window_sample_bytes);
    LD_FIELD(Pop, notice_to_apply_us_sum);
    LD_FIELD(Pop, notice_to_apply_us_max);
    LD_FIELD(Pop, notice_to_apply_n);
    LD_FIELD(Pop, notice_to_pop_us_sum);
    LD_FIELD(Pop, notice_to_pop_us_max);
    LD_FIELD(Pop, notice_to_pop_n);
    LD_FIELD(Apply, apply_overlap_other_lot);
    LD_FIELD(Apply, apply_overlap_ambiguous_lot);
    LD_FIELD(Apply, apply_partial);
    LD_FIELD(Apply, apply_unlinked);
    LD_FIELD(Apply, observation_lost);
    LD_FIELD(Blk, blk_applies);
    LD_FIELD(Blk, blk_applies_over_gpu);
    LD_FIELD(Blk, blk_bytes_range);
    LD_FIELD(Blk, blk_bytes_exact);
    LD_FIELD(Blk, blk_gpu_bytes_covered);
    LD_FIELD(Blk, blk_gpu_bytes_at_risk);
    LD_FIELD(Blk, blk_at_risk_cpu_read);
    LD_FIELD(Blk, blk_at_risk_not_seen_cpu_read);
    LD_FIELD(Blk, blk_at_risk_unknown);
    LD_FIELD(Blk, blk_ambiguous);
    LD_FIELD(Blk, blk_ambiguous_no_record);
    LD_FIELD(Blk, blk_incomplete);
    LD_FIELD(Blk, blk_incomplete_open);
    LD_FIELD(Blk, blk_incomplete_floor);
    LD_FIELD(Blk, blk_incomplete_nomerge);
    LD_FIELD(Blk, blk_incomplete_gpu_bytes);
    LD_FIELD(Blk, blk_affirmed);
    LD_FIELD(Blkcmp, blkcmp_eligible);
    LD_FIELD(Blkcmp, blkcmp_drawn);
    LD_FIELD(Blkcmp, blkcmp_discard_ranges);
    LD_FIELD(Blkcmp, blkcmp_discard_size);
    LD_FIELD(Blkcmp, blkcmp_discard_no_upload);
    LD_FIELD(Blkcmp, blkcmp_discard_no_memory_maps);
    LD_FIELD(Blkcmp, blkcmp_discard_no_single_buffer);
    LD_FIELD(Blkcmp, blkcmp_discard_budget);
    LD_FIELD(Blkcmp, blkcmp_discard_stale);
    LD_FIELD(Blkcmp, blkcmp_compared);
    LD_FIELD(Blkcmp, blkcmp_compared_back);
    LD_FIELD(Blkcmp, blkcmp_compared_front);
    LD_FIELD(Blkcmp, blkcmp_compared_other);
    LD_FIELD(Blkcmp, blkcmp_equal);
    LD_FIELD(Blkcmp, blkcmp_different);
    LD_FIELD(Blkcmp, blkcmp_bytes_compared);
    LD_FIELD(Blkcmp, blkcmp_bytes_different);
    LD_FIELD(Blkcmp, blkcmp_staging_coherent);
    LD_FIELD(Blkcmp, blkcmp_staging_invalidated);
    LD_FIELD(Blkcmp, blkcmp_finish_n);
    LD_FIELD(Blkcmp, blkcmp_finish_us_sum);
    LD_FIELD(Blkcmp, blkcmp_finish_us_max);
    LD_FIELD(Calls, invalidate_calls);
    LD_FIELD(Calls, invalidate_empty);
    LD_FIELD(Calls, gather_overlapped);
    LD_FIELD(Calls, apply_out_of_order);
    LD_FIELD(Calls, invalidate_thread_switches);
    LD_FIELD(Log, log_lines);
    LD_FIELD(Log, log_dropped);
#undef LD_FIELD
    return f;
}

} // namespace VideoCommon::ListDiag
