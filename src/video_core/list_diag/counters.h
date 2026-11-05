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

/// Problem A (the pop window) and problem B (64-byte blocks) have their own counters, as the
/// design requires: fixing one does not fix the other.
struct Counters {
    // The notices (every one ends in exactly one of applied, late, expired, or stays pending).
    Counter notice_registered{};
    Counter notice_applied{};
    Counter notice_late{};
    Counter notice_expired{};
    Counter notice_overflow{};
    Counter notice_ambiguous{};
    // Problem A: a pop over a registered notice that was not applied yet.
    Counter pop_ranges{};
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

enum class Group { Notices, Pop, Apply, Blk, Calls, Log };

struct FieldRef {
    const char* name;
    Group group;
    Counter* counter;
};

/// Every counter, in a fixed order (the report and the violation snapshot walk this list).
inline std::vector<FieldRef> Fields(Counters& c) {
    std::vector<FieldRef> f;
#define LD_FIELD(group, name) f.push_back({#name, Group::group, &c.name})
    LD_FIELD(Notices, notice_registered);
    LD_FIELD(Notices, notice_applied);
    LD_FIELD(Notices, notice_late);
    LD_FIELD(Notices, notice_expired);
    LD_FIELD(Notices, notice_overflow);
    LD_FIELD(Notices, notice_ambiguous);
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
