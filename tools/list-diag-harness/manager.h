// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Model of Core::GPUDirtyMemoryManager (src/core/gpu_dirty_memory_manager.h:26-82), with the
// `Diag` template parameter of docs/list-diag-design.md 3.3.
//
// Diag=false is the code of today, line by line, with the memory orders of today: the test
// "equivalence" runs it against the real header. Diag=true uses seq_cst on `current` and on
// the lot counters, and adds the counters. Nothing else differs.
#pragma once

#include <atomic>
#include <bit>
#include <functional>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "gates.h"

namespace diag {

constexpr size_t kPageBits = 11; // DEVICE_PAGEBITS - 1
constexpr size_t kPageSize = size_t{1} << kPageBits;
constexpr size_t kPageMask = kPageSize - 1;
constexpr size_t kAlignBits = 6;
constexpr size_t kAlignSize = size_t{1} << kAlignBits;
constexpr size_t kAlignMask = kAlignSize - 1;

template <bool Diag>
class Manager {
public:
    Manager() : current{default_transform} {}

    Gates* gates = nullptr;
    // Test only: a Gather that finds another one running returns at once, so that the
    // preexisting race on front_buffer is detected without being executed.
    bool skip_gather_on_overlap = false;

    // Diagnostic state (Diag=true only; never touched with Diag=false).
    std::atomic<u64> lot_started{};
    std::atomic<u64> lot_done{};
    std::atomic<u64> gather_active{};
    std::atomic<u64> apply_started_lot{}; // docs/list-diag-design.md 3.3.1
    std::atomic<u64> violation_floor{};   // lots up to here are ambiguous
    std::atomic<u64> gather_overlapped{};

    void Collect(PAddr address, size_t size) {
        TransformAddress t = BuildTransform(address, size);
        TransformAddress tmp, original;
        do {
            tmp = current.load(kLoad);
            original = tmp;
            if (tmp.address != t.address) {
                if (IsValid(tmp.address)) {
                    std::scoped_lock lk(guard);
                    back_buffer.emplace_back(tmp);
                    current.exchange(t, kExchange);
                    return;
                }
                tmp.address = t.address;
                tmp.mask = 0;
            }
            if ((tmp.mask | t.mask) == tmp.mask) {
                return;
            }
            tmp.mask |= t.mask;
        } while (!current.compare_exchange_weak(original, tmp, kCasSuccess, kCasFailure));
    }

    /// `lot_out` (Diag=true): the lot this call took, set before the callbacks run.
    void Gather(std::function<void(PAddr, size_t)>& callback, u64* lot_out = nullptr) {
        u64 lot = 0;
        if constexpr (Diag) {
            const u64 active_before = gather_active.fetch_add(1, std::memory_order_seq_cst);
            const u64 started = lot_started.fetch_add(1, std::memory_order_seq_cst) + 1;
            if (active_before != 0) {
                gather_overlapped.fetch_add(1, std::memory_order_relaxed);
                RaiseFloor(started);
                if (skip_gather_on_overlap) {
                    gather_active.fetch_sub(1, std::memory_order_seq_cst);
                    if (lot_out != nullptr) {
                        *lot_out = 0;
                    }
                    return;
                }
            }
            Pass(gates, "g.after_inc");
        }
        gather_epoch.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lk(guard);
            TransformAddress t = current.exchange(default_transform, kExchange);
            front_buffer.swap(back_buffer);
            if (IsValid(t.address)) {
                front_buffer.emplace_back(t);
            }
            if constexpr (Diag) {
                // Written after the swap, still under the guard: the guard orders the takings,
                // so the lot number is the order of the takings, not of the increments.
                lot = lot_done.load(std::memory_order_seq_cst) + 1;
                lot_done.store(lot, std::memory_order_seq_cst);
            }
        }
        if constexpr (Diag) {
            if (lot_out != nullptr) {
                *lot_out = lot;
            }
            Pass(gates, "g.before_iter");
        }
        for (auto& transform : front_buffer) {
            size_t offset = 0;
            u64 mask = transform.mask;
            while (mask != 0) {
                const size_t empty_bits = std::countr_zero(mask);
                offset += empty_bits << kAlignBits;
                mask = mask >> empty_bits;

                const size_t continuous_bits = std::countr_one(mask);
                callback((static_cast<PAddr>(transform.address) << kPageBits) + offset,
                         continuous_bits << kAlignBits);
                mask = continuous_bits < kAlignSize ? (mask >> continuous_bits) : 0;
                offset += continuous_bits << kAlignBits;
            }
        }
        front_buffer.clear();
        if constexpr (Diag) {
            gather_active.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    void RaiseFloor(u64 value) {
        u64 old = violation_floor.load(std::memory_order_seq_cst);
        while (old < value &&
               !violation_floor.compare_exchange_weak(old, value, std::memory_order_seq_cst)) {
        }
    }

private:
    struct alignas(8) TransformAddress {
        u32 address;
        u32 mask;
    };

    static constexpr auto kLoad = Diag ? std::memory_order_seq_cst : std::memory_order_acquire;
    static constexpr auto kExchange = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
    static constexpr auto kCasSuccess = Diag ? std::memory_order_seq_cst : std::memory_order_release;
    static constexpr auto kCasFailure = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;

    constexpr static TransformAddress default_transform = {.address = ~0U, .mask = 0U};

    static bool IsValid(PAddr address) {
        return address < (1ULL << 39);
    }

    static u32 CreateMask(size_t top_bit, size_t minor_bit) {
        u32 mask = ~u32(0);
        mask <<= (sizeof(u32) * 8 - top_bit);
        mask >>= (sizeof(u32) * 8 - top_bit);
        mask >>= minor_bit;
        mask <<= minor_bit;
        return mask;
    }

    static TransformAddress BuildTransform(PAddr address, size_t size) {
        const size_t minor_address = address & kPageMask;
        const size_t minor_bit = minor_address >> kAlignBits;
        const size_t top_bit = (minor_address + size + kAlignMask) >> kAlignBits;
        TransformAddress result{};
        result.address = static_cast<u32>(address >> kPageBits);
        result.mask = CreateMask(top_bit, minor_bit);
        return result;
    }

    std::atomic<TransformAddress> current{};
    std::mutex guard;
    std::atomic<u64> gather_epoch{};
    std::vector<TransformAddress> back_buffer;
    std::vector<TransformAddress> front_buffer;
};

} // namespace diag
