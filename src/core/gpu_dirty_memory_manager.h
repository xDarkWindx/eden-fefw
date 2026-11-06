// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "core/device_memory_manager.h"

namespace Core {

/// The grain of the dirty manager: the bytes one bit of a transformation's u32 mask stands for. A transformation always holds
/// 32 grains, so B64 is the transformation of 2 KiB of always, and B4 one of 128 B (docs/t4-coherence-fix-plan.md, 3 and 8,
/// revision 5). B4 is an EXPERIMENTAL mitigation of the neighbour-byte loss (G): it removes the loss only for writes aligned to
/// 4 B in address and size.
enum class DirtyGrain : u8 { B64, B4 };

/// EDEN_DIRTY_GRAIN=4 selects B4; unset, or any other value, selects B64 (the default).
[[nodiscard]] inline DirtyGrain ParseDirtyGrain(const char* value) {
    return value != nullptr && value[0] == '4' && value[1] == '\0' ? DirtyGrain::B4 : DirtyGrain::B64;
}

/// Read once, at the first use, and fixed for the session: every manager made without an explicit grain gets this one.
[[nodiscard]] inline DirtyGrain SessionDirtyGrain() {
    static const DirtyGrain grain = ParseDirtyGrain(std::getenv("EDEN_DIRTY_GRAIN"));
    return grain;
}

[[nodiscard]] constexpr size_t DirtyGrainBytes(DirtyGrain grain) {
    return grain == DirtyGrain::B4 ? 4 : 64;
}

/// How many times a writer took the guard of its manager (the swap of the transformation in `CollectOne`): the cost of the
/// grain on the writer. Always on. Incremented INSIDE the guard, which is taken anyway; none on the CAS path. One cache line
/// per slot, a manager keeping one slot for good (the slots are read and cleared by the statistics report).
struct alignas(64) DirtyGuardSlot {
    std::atomic<u64> takes{};
};
inline constexpr size_t kDirtyGuardSlots = 16;
inline std::array<DirtyGuardSlot, kDirtyGuardSlots> g_dirty_guard_slots;
inline std::atomic<u32> g_dirty_guard_next_slot{};

/// Sum of the takes since the last call, and cleared.
[[nodiscard]] inline u64 TakeDirtyGuardTakes() {
    u64 total = 0;
    for (DirtyGuardSlot& slot : g_dirty_guard_slots) {
        total += slot.takes.exchange(0, std::memory_order_relaxed);
    }
    return total;
}

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
    /// The grain of the session (EDEN_DIRTY_GRAIN, read once).
    GPUDirtyMemoryManager() : GPUDirtyMemoryManager{SessionDirtyGrain()} {}

    /// An explicit grain. The manager is no template (the arrays of managers, the statistics and the diagnostic keep one
    /// type); the grain is a `const bool` next to `current`, and `CollectT` and the iteration of `GatherT` choose between two
    /// instantiations of their bodies by it: each has its shifts and masks as COMPILE-TIME constants, so the 64 B path is the
    /// code of before plus one load and one predictable branch (a fixed value for the whole session), with no variable shift.
    explicit GPUDirtyMemoryManager(DirtyGrain grain)
        : current{default_transform}, fine_grain{grain == DirtyGrain::B4},
          guard_slot{static_cast<u8>(g_dirty_guard_next_slot.fetch_add(1, std::memory_order_relaxed) %
                                     kDirtyGuardSlots)} {
        back_buffer.reserve(256);
        front_buffer.reserve(256);
    }

    ~GPUDirtyMemoryManager() = default;

    void Collect(PAddr address, size_t size) {
        CollectT<false>(address, size);
    }

    /// Whether [address, address + size) passes the end of the transformation of `transform_bits` bits (default: the 2 KiB
    /// of the 64 B grain) that holds `address`. By subtraction: `offset + size` could wrap. The statistics use the same
    /// function.
    [[nodiscard]] static constexpr bool CrossesTransform(PAddr address, size_t size,
                                                         size_t transform_bits = kDefaultPageBits) {
        const size_t transform = size_t{1} << transform_bits;
        return size > transform - (address & (transform - 1));
    }

    [[nodiscard]] DirtyGrain Grain() const {
        return fine_grain ? DirtyGrain::B4 : DirtyGrain::B64;
    }

    /// `Diag = false` is the code of before the diagnostic, with its memory orders. `Diag = true`
    /// only changes the orders of the operations on `current` to seq_cst (docs/list-diag-design.md,
    /// 3.3); nothing else.
    template <bool Diag, class Hooks = DirtyNoHooks>
    void CollectT(PAddr address, size_t size) {
        if (size == 0) {
            return;
        }
        if (fine_grain) {
            CollectSplit<Diag, Hooks, kAlignBits4>(address, size);
        } else {
            CollectSplit<Diag, Hooks, kAlignBits64>(address, size);
        }
    }

private:
    /// CollectT for one grain, `AlignBits` a compile-time constant (2 or 6; a transformation is 32 grains).
    template <bool Diag, class Hooks, size_t AlignBits>
    void CollectSplit(PAddr address, size_t size) {
        constexpr size_t page_bits = AlignBits + 5;
        constexpr size_t page_size = size_t{1} << page_bits;
        constexpr size_t page_mask = page_size - 1;
        // One transformation covers `page_size` bytes (2 KiB, or 128 B with the 4 B grain); a write that crosses its end used
        // to build a mask with a negative shift (docs/gpu-contract-inventory.md, 7.4). Subtraction, not
        // `offset + size`, so that a huge size cannot wrap. A write that fits takes the old path
        // without a loop.
        if (size <= page_size - (address & page_mask)) {
            CollectOne<Diag, Hooks, AlignBits>(address, size);
            return;
        }
        for (;;) {
            const size_t piece = (std::min)(size, page_size - (address & page_mask));
            CollectOne<Diag, Hooks, AlignBits>(address, piece);
            address += piece;
            size -= piece;
            if (size == 0) {
                return;
            }
            Hooks::At("c.between_pieces");
        }
    }

    /// The body of CollectT before the split, unchanged (same memory orders): `size` fits in one
    /// transformation.
    template <bool Diag, class Hooks, size_t AlignBits>
    void CollectOne(PAddr address, size_t size) {
        constexpr auto load_order = Diag ? std::memory_order_seq_cst : std::memory_order_acquire;
        constexpr auto exchange_order = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
        constexpr auto cas_success = Diag ? std::memory_order_seq_cst : std::memory_order_release;
        constexpr auto cas_failure = Diag ? std::memory_order_seq_cst : std::memory_order_relaxed;
        TransformAddress t = BuildTransform<AlignBits>(address, size);
        TransformAddress tmp, original;
        do {
            tmp = current.load(load_order);
            original = tmp;
            if (tmp.address != t.address) {
                if (HasWrites(tmp)) {
                    std::scoped_lock lk(guard);
                    // Writer cost of the grain (see DirtyGuardSlot): under the guard, never on the CAS path. The slot of a manager
                    // is its own (at most kDirtyGuardSlots managers are alive together) and the guard orders the writers
                    // of this manager, so a load and a store do: no lock-prefixed operation.
                    DirtyGuardSlot& slot = g_dirty_guard_slots[guard_slot];
                    slot.takes.store(slot.takes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
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
            Hooks::At("c.before_cas"); // test stop point (folds away): between the read of `current` and the CAS
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
        if (fine_grain) {
            Deliver<kAlignBits4>(callback);
        } else {
            Deliver<kAlignBits64>(callback);
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

    /// Bits of a mask: one transformation is always 32 grains, whatever the grain is.
    constexpr static size_t mask_width = 32;
    constexpr static size_t kAlignBits64 = 6; ///< grain 64 B, transformation 2 KiB
    constexpr static size_t kAlignBits4 = 2;  ///< grain 4 B, transformation 128 B
    /// The transformation of the 64 B grain: 2 KiB.
    constexpr static size_t kDefaultPageBits = DEVICE_PAGEBITS - 1;
    static_assert(kDefaultPageBits == kAlignBits64 + 5, "32 grains of 64 B");

    /// The ranges of `front_buffer`, for one grain (`AlignBits` constant).
    template <size_t AlignBits>
    void Deliver(std::function<void(PAddr, size_t)>& callback) {
        constexpr size_t page_bits = AlignBits + 5;
        for (auto& transform : front_buffer) {
            size_t offset = 0;
            u64 mask = transform.mask;
            while (mask != 0) {
                const size_t empty_bits = std::countr_zero(mask);
                offset += empty_bits << AlignBits;
                mask = mask >> empty_bits;

                const size_t continuous_bits = std::countr_one(mask);
                callback((static_cast<PAddr>(transform.address) << page_bits) + offset, continuous_bits << AlignBits);
                // The condition is about the WIDTH OF THE MASK (32 bits), not the size of the grain in bytes: with the
                // grain in bytes (64) it was always true by coincidence, and with a 4 B grain it dropped every run after
                // the first (docs/t4-coherence-fix-plan.md, 8, revision 1, item 1). A run of all 32 bits ends the mask.
                mask = continuous_bits < mask_width ? (mask >> continuous_bits) : 0;
                offset += continuous_bits << AlignBits;
            }
        }
    }

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

    template <size_t AlignBits>
    TransformAddress BuildTransform(PAddr address, size_t size) {
        constexpr size_t page_bits = AlignBits + 5;
        constexpr size_t page_mask = (size_t{1} << page_bits) - 1;
        constexpr size_t align_mask = (size_t{1} << AlignBits) - 1;
        const size_t minor_address = address & page_mask;
        const size_t minor_bit = minor_address >> AlignBits;
        const size_t top_bit = (minor_address + size + align_mask) >> AlignBits;
        TransformAddress result{};
        result.address = static_cast<u32>(address >> page_bits);
        result.mask = CreateMask<u32>(top_bit, minor_bit);
        return result;
    }

    std::atomic<TransformAddress> current{};
    /// The grain, fixed at construction (next to `current`: the same cache line).
    const bool fine_grain; ///< true: 4 B grain (transformation 128 B); false: 64 B (2 KiB)
    const u8 guard_slot;   ///< Which DirtyGuardSlot counts this manager's guard takes
    std::mutex guard;
    std::atomic<u64> gather_epoch{};
    std::vector<TransformAddress> back_buffer;
    std::vector<TransformAddress> front_buffer;
};

} // namespace Core
