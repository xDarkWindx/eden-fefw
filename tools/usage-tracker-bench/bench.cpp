// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Microbenchmark of UsageTracker::Track / IsUsed: the tracker of T4.1 (VideoCommon::UsageTracker) against the FROZEN copy of
// the one before it (LegacyUsage::UsageTracker, src/tests/video_core/usage_tracker_legacy.h). Investigation of
// docs/t4-coherence-fix-plan.md, 9.6. It changes nothing in the emulator.
//
// Only ranges where the OLD tracker is defined are run on it (no undefined shift, so no UBSan and no exemption):
// size >= 64 and, when the range crosses a 4 KiB word, the end falls at least 64 B into its word. Build without sanitizers
// (tools/usage-tracker-bench/run.sh). Reports ns per call (median of the passes), and ns per Reset.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "video_core/buffer_cache/usage_tracker.h"
#include "tests/video_core/usage_tracker_legacy.h"

namespace {

struct Range {
    u64 offset;
    u64 size;
};

using Clock = std::chrono::steady_clock;

template <class T>
inline void Keep(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

/// The old tracker is defined here (see the header comment).
bool OldDefined(const Range& r) {
    constexpr u64 page = 4096;
    if (r.size < 64) {
        return false;
    }
    const bool one_word = (r.offset / page) == ((r.offset + r.size) / page);
    return one_word || ((r.offset + r.size) % page) >= 64;
}

struct Scenario {
    std::string name;
    u64 buffer;
    std::vector<Range> ranges;   // one pass (the order a draw sequence would bind them)
    bool old_runs;               // false: not every range is defined for the old tracker
    std::string note;
};

template <class Tracker>
double MedianNsPerTrack(const Scenario& sc, double* reset_ns) {
    constexpr int kPasses = 9;
    constexpr int kRepeat = 200; // draws between resets
    std::vector<double> track(kPasses), reset(kPasses);
    Tracker tracker{sc.buffer};
    for (int pass = 0; pass < kPasses + 2; ++pass) {
        const auto t0 = Clock::now();
        for (int rep = 0; rep < kRepeat; ++rep) {
            for (const Range& r : sc.ranges) {
                tracker.Track(r.offset, r.size);
            }
        }
        const auto t1 = Clock::now();
        tracker.Reset();
        const auto t2 = Clock::now();
        if (pass >= 2) {
            track[pass - 2] = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                              (static_cast<double>(kRepeat) * static_cast<double>(sc.ranges.size()));
            reset[pass - 2] = std::chrono::duration<double, std::nano>(t2 - t1).count();
        }
    }
    std::sort(track.begin(), track.end());
    std::sort(reset.begin(), reset.end());
    *reset_ns = reset[kPasses / 2];
    return track[kPasses / 2];
}

/// Every range of the scenario is tracked once per kRepeat; the first pass over a fresh tracker (after Reset) misses the
/// `recent` cache for a list longer than 16 ranges, and later passes hit it only for the ranges still in the 16 slots.
template <class Tracker>
double MedianNsPerIsUsed(const Scenario& sc, const std::vector<Range>& tracked, const std::vector<Range>& queries,
                         u64& used) {
    constexpr int kPasses = 9;
    constexpr int kRepeat = 200;
    Tracker tracker{sc.buffer};
    for (const Range& r : tracked) {
        tracker.Track(r.offset, r.size);
    }
    std::vector<double> times(kPasses);
    used = 0;
    for (int pass = 0; pass < kPasses + 2; ++pass) {
        u64 count = 0;
        const auto t0 = Clock::now();
        for (int rep = 0; rep < kRepeat; ++rep) {
            for (const Range& q : queries) {
                count += tracker.IsUsed(q.offset, q.size) ? 1 : 0;
            }
        }
        const auto t1 = Clock::now();
        Keep(count);
        used = count / kRepeat;
        if (pass >= 2) {
            times[pass - 2] = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                              (static_cast<double>(kRepeat) * static_cast<double>(queries.size()));
        }
    }
    std::sort(times.begin(), times.end());
    return times[kPasses / 2];
}

std::vector<Range> Gen(std::mt19937_64& rng, size_t count, u64 buffer, u64 align, u64 min_size, u64 max_size) {
    std::vector<Range> out;
    while (out.size() < count) {
        const u64 size = min_size + rng() % (max_size - min_size + 1);
        if (size >= buffer) {
            continue;
        }
        const u64 offset = (rng() % (buffer - size)) / align * align;
        out.push_back({offset, size});
    }
    return out;
}

std::vector<Range> OnlyOldDefined(std::vector<Range> in) {
    std::erase_if(in, [](const Range& r) { return !OldDefined(r); });
    return in;
}

} // namespace

int main() {
    std::mt19937_64 rng{0xb3c4};
    std::vector<Scenario> scenarios;

    // MarkUsage callers of the table of revision 3 (t4-coherence-fix-plan.md, 8): ranges are bytes, offsets are in the buffer.
    {   // Uniforms (buffer_cache.h:1073): small, classic cached path only. Distinct ranges: misses `recent`.
        Scenario s{"uniform small, 256 distinct", 8 << 20, OnlyOldDefined(Gen(rng, 400, 8 << 20, 256, 64, 4096)), true, ""};
        scenarios.push_back(s);
    }
    {   // The same few uniforms every draw: hits `recent`.
        std::vector<Range> few = Gen(rng, 8, 8 << 20, 256, 64, 4096);
        Scenario s{"uniform small, 8 repeated (recent hit)", 8 << 20, OnlyOldDefined(few), true, ""};
        scenarios.push_back(s);
    }
    {   // Storage buffers carved from one pool (buffer_cache.h:1239/1270): each its own offset and size, up to hundreds of KiB.
        Scenario s{"storage segmented 64 B..256 KiB", 64 << 20, OnlyOldDefined(Gen(rng, 200, 64 << 20, 64, 64, 256 << 10)), true,
                   ""};
        scenarios.push_back(s);
    }
    {   // Storage buffers, 12 repeated (hit) and 40 distinct (miss).
        Scenario s{"storage 12 repeated (recent hit)", 64 << 20, OnlyOldDefined(Gen(rng, 12, 64 << 20, 64, 4096, 1 << 20)), true,
                   ""};
        scenarios.push_back(s);
    }
    {   // Vertex buffers (:902, :945): large, repeated. Hit.
        Scenario s{"vertex 4..32 MiB, 3 repeated (recent hit)", 64 << 20, OnlyOldDefined(Gen(rng, 3, 64 << 20, 4096, 4 << 20, 32 << 20)),
                   true, ""};
        scenarios.push_back(s);
    }
    {   // Vertex buffers, 24 distinct large ranges cycling: more than the 16 slots, so every Track misses.
        Scenario s{"vertex 1..16 MiB, 24 cycling (recent miss)", 64 << 20, OnlyOldDefined(Gen(rng, 24, 64 << 20, 64, 1 << 20, 16 << 20)),
                   true, ""};
        scenarios.push_back(s);
    }
    {   // Vertex buffers: one big range changing by a few bytes every draw (a miss each time, the worst case for the miss path).
        std::vector<Range> v;
        for (u64 i = 0; i < 64; ++i) {
            v.push_back({4096 + i * 64, 8u << 20});
        }
        Scenario s{"vertex 8 MiB, offset changes each draw (miss)", 64 << 20, OnlyOldDefined(v), true, ""};
        scenarios.push_back(s);
    }
    {   // Ranges ending near a 4 KiB edge: 64 B before and 64 B after (defined for the old tracker; ending EXACTLY on it is not).
        std::vector<Range> v;
        for (u64 i = 0; i < 256; ++i) {
            const u64 edge = (1 + rng() % 4000) * 4096;
            const u64 size = 64 + (rng() % 60) * 64;
            v.push_back({edge - size - ((i & 1) ? 64 : 0) + ((i & 2) ? 128 : 0), size + ((i & 1) ? 64 : 0)});
        }
        Scenario s{"ends 64 B near a 4 KiB edge", 32 << 20, OnlyOldDefined(v), true, ""};
        scenarios.push_back(s);
    }

    std::printf("%-48s %7s | %10s %10s | %9s %9s\n", "Track", "ranges", "new ns", "old ns", "new Reset", "old Reset");
    for (const Scenario& s : scenarios) {
        double new_reset = 0, old_reset = 0;
        const double n = MedianNsPerTrack<VideoCommon::UsageTracker>(s, &new_reset);
        const double o = MedianNsPerTrack<LegacyUsage::UsageTracker>(s, &old_reset);
        std::printf("%-48s %7zu | %10.2f %10.2f | %7.0f ns %7.0f ns\n", s.name.c_str(), s.ranges.size(), n, o, new_reset, old_reset);
    }

    // IsUsed: the dst ranges of buffer uploads (CanReorderUpload: IsRegionUsed(copy.dst_offset, copy.size)) against a tracker
    // that already holds a draw's worth of ranges. Most answers are "not used": the whole range is scanned.
    std::printf("\n%-48s %7s | %10s %10s | %9s %9s\n", "IsUsed (dst of uploads)", "queries", "new ns", "old ns", "new true", "old true");
    struct Case {
        const char* name;
        u64 buffer;
        u64 min_size, max_size, align;
        size_t tracked;
        u64 track_min, track_max;
    };
    for (const Case& c : {Case{"upload 64 B..4 KiB, 8 tracked", 8 << 20, 64, 4096, 64, 8, 64, 4096},
                          Case{"upload 4..64 KiB, 40 tracked small", 8 << 20, 4096, 64 << 10, 64, 40, 64, 4096},
                          Case{"upload 64 KiB..1 MiB, 12 tracked 1 MiB", 64 << 20, 64 << 10, 1 << 20, 4096, 12, 64 << 10, 1 << 20},
                          Case{"upload 1..8 MiB, 3 tracked 4..32 MiB", 64 << 20, 1 << 20, 8 << 20, 4096, 3, 4 << 20, 32 << 20}}) {
        Scenario s{c.name, c.buffer, {}, true, ""};
        const std::vector<Range> tracked = OnlyOldDefined(Gen(rng, c.tracked, c.buffer, 64, c.track_min, c.track_max));
        const std::vector<Range> queries = OnlyOldDefined(Gen(rng, 300, c.buffer, c.align, c.min_size, c.max_size));
        u64 used_new = 0, used_old = 0;
        const double n = MedianNsPerIsUsed<VideoCommon::UsageTracker>(s, tracked, queries, used_new);
        const double o = MedianNsPerIsUsed<LegacyUsage::UsageTracker>(s, tracked, queries, used_old);
        std::printf("%-48s %7zu | %10.2f %10.2f | %9llu %9llu\n", c.name, queries.size(), n, o,
                    static_cast<unsigned long long>(used_new), static_cast<unsigned long long>(used_old));
    }
    return 0;
}
