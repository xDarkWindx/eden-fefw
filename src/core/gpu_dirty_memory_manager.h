// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "core/device_memory_manager.h"

namespace Core {

/// Stop points for the tests of the diagnostic (EDEN_LIST_DIAG, docs/list-diag-design.md). A test
/// supplies a type with the same members whose `At` blocks on a named point; here nothing happens
/// and everything folds away.
struct DirtyNoHooks {
    static constexpr bool kTest = false;
    static void At(const char*) {}
    static bool SkipGatherOnOverlap() {
        return false;
    }
};

/// Per-core state of the diagnostic: which lot a Gather took and which lot is being applied
/// (docs/list-diag-design.md, 3.3). It lives OUTSIDE the manager, one cache line per core and only
/// allocated when the diagnostic is on, so the manager keeps the layout it always had. Every
/// access is seq_cst: the argument that a notice belongs to lot `lot_done + 1` rests on the total
/// order of these operations and of `current`.
struct alignas(64) GatherLotState {
    std::atomic<u64> lot_started{};       ///< Gathers that began (incremented before the guard)
    std::atomic<u64> lot_done{};          ///< Lots taken: the order of the takings, written under the guard
    std::atomic<u64> gather_active{};     ///< Gathers between their entry and the end of the iteration
    std::atomic<u64> apply_started_lot{}; ///< "Applying up to this lot", written before the sweep
    std::atomic<u64> violation_floor{};   ///< Lots up to here are suspect (precondition P0 broken)
    std::atomic<u64> gather_overlapped{}; ///< Gathers that began while another was running
};
static_assert(sizeof(GatherLotState) == 64);

class GPUDirtyMemoryManager {
public:
    GPUDirtyMemoryManager() : current{default_transform} {
        back_buffer.reserve(256);
        front_buffer.reserve(256);
    }

    ~GPUDirtyMemoryManager() = default;

    void Collect(PAddr address, size_t size) {
        CollectT<false>(address, size);
    }

    /// Whether [address, address + size) passes the end of the transformation that holds `address`
    /// (2 KiB). By subtraction: `offset + size` could wrap. The statistics use the same function.
    [[nodiscard]] static constexpr bool CrossesTransform(PAddr address, size_t size) {
        return size > page_size - (address & page_mask);
    }

    /// `Diag = false` is the code of before the diagnostic, with its memory orders. `Diag = true`
    /// only changes the orders of the operations on `current` to seq_cst (docs/list-diag-design.md,
    /// 3.3); nothing else.
    template <bool Diag, class Hooks = DirtyNoHooks>
    void CollectT(PAddr address, size_t size) {
        if (size == 0) {
            return;
        }
        // One transformation covers `page_size` bytes (2 KiB); a write that crosses its end used to
        // build a mask with a negative shift (docs/gpu-contract-inventory.md, 7.4). Subtraction, not
        // `offset + size`, so that a huge size cannot wrap. A write that fits takes the old path
        // without a loop.
        if (!CrossesTransform(address, size)) {
            CollectOne<Diag, Hooks>(address, size);
            return;
        }
        for (;;) {
            const size_t piece = (std::min)(size, page_size - (address & page_mask));
            CollectOne<Diag, Hooks>(address, piece);
            address += piece;
            size -= piece;
            if (size == 0) {
                return;
            }
            Hooks::At("c.between_pieces");
        }
    }

private:
    /// The body of CollectT before the split, unchanged (same memory orders): `size` fits in one
    /// transformation.
    template <bool Diag, class Hooks>
    void CollectOne(PAddr address, size_t size) {
        constexpr auto load_order = Diag ? std::memory_order_seq_cst : std::memory_order_acquire;
        constexpr auto exchange_order = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
        constexpr auto cas_success = Diag ? std::memory_order_seq_cst : std::memory_order_release;
        constexpr auto cas_failure = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
        TransformAddress t = BuildTransform(address, size);
        TransformAddress tmp, original;
        do {
            tmp = current.load(load_order);
            original = tmp;
            if (tmp.address != t.address) {
                if (HasWrites(tmp)) {
                    std::scoped_lock lk(guard);
                    back_buffer.emplace_back(tmp);
                    current.exchange(t, exchange_order);
                    return;
                }
                tmp.address = t.address;
                tmp.mask = 0;
            }
            if ((tmp.mask | t.mask) == tmp.mask) {
                return;
            }
            tmp.mask |= t.mask;
        } while (!current.compare_exchange_weak(original, tmp, cas_success, cas_failure));
    }

public:

    /// Bumped by every Gather: what a writer remembered about pages it collected before is stale
    /// once their writes were applied (core/memory.cpp, EDEN_CPU_WRITE_PAGES).
    [[nodiscard]] u64 GatherEpoch() const {
        return gather_epoch.load(std::memory_order_relaxed);
    }

    void Gather(std::function<void(PAddr, size_t)>& callback) {
        GatherT<false>(callback, nullptr, nullptr);
    }

    /// `Diag = false` is the Gather of before the diagnostic (`lots` and `lot_out` unused).
    /// `Diag = true` adds the lot bookkeeping of docs/list-diag-design.md, 3.3: `lot_started`
    /// before the guard, `lot_done` after the swap and still under it, `gather_active` around the
    /// whole call (the iteration of `front_buffer` is outside the guard, so two Gathers at once
    /// on one manager are a race that exists today; the diagnostic only counts it). `lot_out`
    /// is set before the callbacks run.
    template <bool Diag, class Hooks = DirtyNoHooks>
    void GatherT(std::function<void(PAddr, size_t)>& callback, GatherLotState* lots, u64* lot_out) {
        constexpr auto exchange_order = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
        u64 lot = 0;
        if constexpr (Diag) {
            const u64 active_before = lots->gather_active.fetch_add(1, std::memory_order_seq_cst);
            const u64 started = lots->lot_started.fetch_add(1, std::memory_order_seq_cst) + 1;
            if (active_before != 0) {
                lots->gather_overlapped.fetch_add(1, std::memory_order_seq_cst);
                RaiseFloor(*lots, started);
                if constexpr (Hooks::kTest) {
                    if (Hooks::SkipGatherOnOverlap()) {
                        // Test only: the race on front_buffer itself is not executed.
                        lots->gather_active.fetch_sub(1, std::memory_order_seq_cst);
                        if (lot_out != nullptr) {
                            *lot_out = 0;
                        }
                        return;
                    }
                }
            }
            Hooks::At("g.after_inc");
        }
        gather_epoch.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lk(guard);
            TransformAddress t = current.exchange(default_transform, exchange_order);
            front_buffer.swap(back_buffer);
            if (HasWrites(t)) {
                front_buffer.emplace_back(t);
            }
            if constexpr (Diag) {
                // After the swap, still under the guard: the guard orders the takings, so the lot
                // number is the order of the takings, not of the increments.
                lot = lots->lot_done.load(std::memory_order_seq_cst) + 1;
                lots->lot_done.store(lot, std::memory_order_seq_cst);
            }
        }
        if constexpr (Diag) {
            if (lot_out != nullptr) {
                *lot_out = lot;
            }
            Hooks::At("g.before_iter");
        }
        for (auto& transform : front_buffer) {
            size_t offset = 0;
            u64 mask = transform.mask;
            while (mask != 0) {
                const size_t empty_bits = std::countr_zero(mask);
                offset += empty_bits << align_bits;
                mask = mask >> empty_bits;

                const size_t continuous_bits = std::countr_one(mask);
                callback((static_cast<PAddr>(transform.address) << page_bits) + offset,
                         continuous_bits << align_bits);
                mask = continuous_bits < align_size ? (mask >> continuous_bits) : 0;
                offset += continuous_bits << align_bits;
            }
        }
        front_buffer.clear();
        if constexpr (Diag) {
            lots->gather_active.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    static void RaiseFloor(GatherLotState& lots, u64 value) {
        u64 old = lots.violation_floor.load(std::memory_order_seq_cst);
        while (old < value &&
               !lots.violation_floor.compare_exchange_weak(old, value, std::memory_order_seq_cst)) {
        }
    }

private:
    struct alignas(8) TransformAddress {
        u32 address;
        u32 mask;
    };

    constexpr static size_t page_bits = DEVICE_PAGEBITS - 1;
    constexpr static size_t page_size = 1ULL << page_bits;
    constexpr static size_t page_mask = page_size - 1;

    constexpr static size_t align_bits = 6U;
    constexpr static size_t align_size = 1U << align_bits;
    constexpr static size_t align_mask = align_size - 1;
    constexpr static TransformAddress default_transform = {.address = ~0U, .mask = 0U};

    /// A transformation holds writes iff its mask is not zero; the empty one is `default_transform` (what
    /// `current` holds after a Gather, and at the start). Identified by the MASK, never by the address: the
    /// marker's address (~0U) is also a legitimate address field, and a write there has a non-zero mask
    /// (docs/t4-coherence-fix-plan.md, 3 and 8, item 4). The old `IsValid` tested `address < 2^39` on the
    /// u32 field of the transformation, so every transformation, the empty one included, counted as valid,
    /// and an empty one was queued in back_buffer and front_buffer for nothing.
    [[nodiscard]] static constexpr bool HasWrites(const TransformAddress& transform) {
        return transform.mask != 0;
    }

    template <typename T>
    T CreateMask(size_t top_bit, size_t minor_bit) {
        T mask = ~T(0);
        mask <<= (sizeof(T) * 8 - top_bit);
        mask >>= (sizeof(T) * 8 - top_bit);
        mask >>= minor_bit;
        mask <<= minor_bit;
        return mask;
    }

    TransformAddress BuildTransform(PAddr address, size_t size) {
        const size_t minor_address = address & page_mask;
        const size_t minor_bit = minor_address >> align_bits;
        const size_t top_bit = (minor_address + size + align_mask) >> align_bits;
        TransformAddress result{};
        result.address = static_cast<u32>(address >> page_bits);
        result.mask = CreateMask<u32>(top_bit, minor_bit);
        return result;
    }

    std::atomic<TransformAddress> current{};
    std::mutex guard;
    std::atomic<u64> gather_epoch{};
    std::vector<TransformAddress> back_buffer;
    std::vector<TransformAddress> front_buffer;
};

} // namespace Core
