// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"

// Size and alignment of the HELD guest writes (docs/t4-coherence-fix-plan.md, section 4, revision 4). Pure functions,
// so the harness checks the formulas; the counters are filled in core/memory.cpp (behind BackStats::Enabled()) and
// reported every 4 s by back_thread_stats.cpp.
//
// This sizes the writes OUTSIDE the guarantee of O1 (the ones not aligned to the grain) and the estimated cost in
// pieces and grains. It does NOT measure how many neighbouring bytes hold diverging GPU data, nor how many would be
// lost for real.
namespace VideoCommon::HeldShape {

/// The candidate grains, in bytes, and the 64 B baseline (today: a transformation of 2 KiB = 32 x 64 B).
inline constexpr std::array<u64, 2> kGrains{4, 8};
inline constexpr u64 kBaselineGrain = 64;
/// Slots of the cost counters: grain 4, grain 8, baseline 64.
inline constexpr size_t kCostSlots = 3;
inline constexpr std::array<u64, kCostSlots> kCostGrains{4, 8, kBaselineGrain};

/// The transformation `CollectT` would use for a grain: 32 x grain (the baseline: 32 x 64 = 2048 B, the one of today).
[[nodiscard]] constexpr u64 TransformOf(u64 grain) {
    return 32 * grain;
}

/// Pieces `CollectT` would make of a NON-EMPTY write: 1 + ((address % T) + size - 1) / T, with T = `transform`.
/// Computed as 1 + q + carry (q = (size - 1) / T), which is the same value without the sum `(address % T) + size - 1`
/// that could wrap for a huge size. Size zero is the caller's to handle: this returns 0 for it, never a formula.
[[nodiscard]] constexpr u64 PiecesOf(u64 address, u64 size, u64 transform) {
    if (size == 0) {
        return 0;
    }
    const u64 rest = size - 1;
    return 1 + rest / transform + ((address % transform) + rest % transform >= transform ? 1 : 0);
}

/// Grains a NON-EMPTY write touches: ((address + size - 1) / g) - (address / g) + 1, written as
/// 1 + ((address % g) + size - 1) / g so that `address + size` cannot wrap. Zero for size zero.
[[nodiscard]] constexpr u64 GrainsTouched(u64 address, u64 size, u64 grain) {
    if (size == 0) {
        return 0;
    }
    const u64 rest = size - 1;
    return 1 + rest / grain + ((address % grain) + rest % grain >= grain ? 1 : 0);
}

/// Overlapping categories of one grain (a piece is in exactly one).
enum class Align : u8 { Both, AddressOnly, SizeOnly, Neither, Count };

[[nodiscard]] constexpr Align AlignOf(u64 address, u64 size, u64 grain) {
    const bool address_ok = address % grain == 0;
    const bool size_ok = size % grain == 0;
    return address_ok ? (size_ok ? Align::Both : Align::AddressOnly)
                      : (size_ok ? Align::SizeOnly : Align::Neither);
}

/// Everything the counters take from one held piece. `empty` set: nothing else is meaningful (zero size is counted
/// apart, before any formula).
struct Shape {
    bool empty{};
    bool under_4{};
    std::array<Align, kGrains.size()> align{};
    std::array<u64, kCostSlots> pieces{};
    std::array<u64, kCostSlots> grains{};
};

[[nodiscard]] constexpr Shape ShapeOf(u64 address, u64 size) {
    Shape shape;
    if (size == 0) {
        shape.empty = true;
        return shape;
    }
    shape.under_4 = size < 4;
    for (size_t i = 0; i < kGrains.size(); ++i) {
        shape.align[i] = AlignOf(address, size, kGrains[i]);
    }
    for (size_t i = 0; i < kCostSlots; ++i) {
        shape.pieces[i] = PiecesOf(address, size, TransformOf(kCostGrains[i]));
        shape.grains[i] = GrainsTouched(address, size, kCostGrains[i]);
    }
    return shape;
}

/// The counters. `Sources` is the number of sources (BackStats::DirtySource::Count): the same sources as the D1.
template <size_t Sources>
struct Counters {
    std::atomic<u64> empty{};   ///< size zero (apart, no formula)
    std::atomic<u64> pieces{};  ///< non-empty held pieces
    std::atomic<u64> bytes{};
    std::array<std::atomic<u64>, Sources> pieces_by_source{};
    std::array<std::atomic<u64>, Sources> bytes_by_source{};
    std::atomic<u64> under_4{};
    std::array<std::array<std::atomic<u64>, static_cast<size_t>(Align::Count)>, kGrains.size()> align{};
    std::array<std::atomic<u64>, kCostSlots> cost_pieces{};
    std::array<std::atomic<u64>, kCostSlots> cost_grains{};

    void Add(u64 address, u64 size, size_t source) {
        const Shape shape = ShapeOf(address, size);
        if (shape.empty) {
            empty.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pieces.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);
        pieces_by_source[source].fetch_add(1, std::memory_order_relaxed);
        bytes_by_source[source].fetch_add(size, std::memory_order_relaxed);
        if (shape.under_4) {
            under_4.fetch_add(1, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kGrains.size(); ++i) {
            align[i][static_cast<size_t>(shape.align[i])].fetch_add(1, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kCostSlots; ++i) {
            cost_pieces[i].fetch_add(shape.pieces[i], std::memory_order_relaxed);
            cost_grains[i].fetch_add(shape.grains[i], std::memory_order_relaxed);
        }
    }
};

} // namespace VideoCommon::HeldShape
