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
#include <span>
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

/// The condition of EDEN_BACK_THREAD_STATS (the statistics are on): the variable is set, not empty and does not start with '0'.
/// One definition for the report (video_core/back_thread_stats.cpp) and for the counter of the guard below.
[[nodiscard]] inline bool StatsEnvEnabled(const char* value) {
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Read once, at the first use: whether the statistics are on for the session. The counter of the guard counts only then.
[[nodiscard]] inline bool SessionDirtyStats() {
    static const bool enabled = StatsEnvEnabled(std::getenv("EDEN_BACK_THREAD_STATS"));
    return enabled;
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
    /// The grain of the session (EDEN_DIRTY_GRAIN, read once) and the statistics of the session.
    GPUDirtyMemoryManager() : GPUDirtyMemoryManager{SessionDirtyGrain(), SessionDirtyStats()} {}

    /// An explicit grain. The manager is no template (the arrays of managers, the statistics and the diagnostic keep one
    /// type); the grain is a `const bool` next to `current`, and `CollectT` and the iteration of `GatherT` choose between two
    /// instantiations of their bodies by it: each has its shifts and masks as COMPILE-TIME constants, so the 64 B path is the
    /// code of before plus one load and one predictable branch (a fixed value for the whole session), with no variable shift.
    ///
    /// `count_guard_takes` turns on the counter of the guard takes (the writer cost of the grain, `GuardTakes`): fixed at
    /// construction like the grain, and tested INSIDE the guard, so that without the statistics a writer does nothing more.
    explicit GPUDirtyMemoryManager(DirtyGrain grain, bool count_guard_takes = SessionDirtyStats())
        : current{default_transform}, fine_grain{grain == DirtyGrain::B4}, count_guard{count_guard_takes} {
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

    /// How many times a writer took the guard of THIS manager (the swap of the transformation in `CollectOne`): the writer
    /// cost of the grain. CUMULATIVE, owned by the manager, never reset: a reader keeps its own previous value and takes the
    /// difference (DirtyGuardReader). Written only inside the guard, so there is one writer at a time (W1) and a load and a
    /// store are exact; read by any thread, relaxed. Counts only when the manager was made with the statistics on.
    [[nodiscard]] u64 GuardTakes() const {
        return guard_takes.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool CountsGuardTakes() const {
        return count_guard;
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
                    // Writer cost of the grain (GuardTakes): under the guard, never on the CAS path, and only with the statistics
                    // on. The guard orders the writers of this manager (W1), so a load and a store are exact.
                    if (count_guard) {
                        const u64 takes = guard_takes.load(std::memory_order_relaxed);
                        Hooks::At("c.guard_count"); // test stop point (folds away): between the load and the store
                        guard_takes.store(takes + 1, std::memory_order_relaxed);
                    }
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
    const bool count_guard; ///< the statistics are on: count the guard takes
    std::mutex guard;
    std::atomic<u64> gather_epoch{};
    std::atomic<u64> guard_takes{}; ///< cumulative; written only inside `guard`
    std::vector<TransformAddress> back_buffer;
    std::vector<TransformAddress> front_buffer;
};

/// Which managers the statistics report reads: the ones the session's System owns (an array that lives as long as the System),
/// registered explicitly at its start and unregistered at its end. No manager registers itself, so a manager made by a test or a
/// tool is never summed in by accident, and there is no allocator of slots. The report reads under `mutex_`, and so does
/// Unregister: once it returns, the report no longer touches the array that is about to be destroyed.
class DirtyGuardRegistry {
public:
    /// Returns the serial of this registration (never reused), the identity a reader keeps its previous values by.
    u64 Register(std::span<const GPUDirtyMemoryManager> managers) {
        std::scoped_lock lock{mutex_};
        const u64 serial = next_serial_++;
        entries_.push_back({serial, managers});
        return serial;
    }

    void Unregister(u64 serial) {
        std::scoped_lock lock{mutex_};
        std::erase_if(entries_, [serial](const Entry& entry) { return entry.serial == serial; });
    }

    /// `visit(serial, index, manager)` for every registered manager, under the registry's lock.
    template <class Visit>
    void ForEach(Visit&& visit) const {
        std::scoped_lock lock{mutex_};
        for (const Entry& entry : entries_) {
            for (size_t index = 0; index < entry.managers.size(); ++index) {
                visit(entry.serial, index, entry.managers[index]);
            }
        }
    }

private:
    struct Entry {
        u64 serial;
        std::span<const GPUDirtyMemoryManager> managers;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    u64 next_serial_{1};
};

/// The registry of the session: the System registers its array here.
[[nodiscard]] inline DirtyGuardRegistry& SessionDirtyGuardRegistry() {
    static DirtyGuardRegistry registry;
    return registry;
}

/// What the statistics report keeps: the cumulative value last read of each registered manager, by (registration, index). The
/// counters are never reset; the report takes the DIFFERENCE since its previous reading. A manager that was unregistered is
/// forgotten here, and one registered later (even at the same address) is another registration, so counts never mix.
class DirtyGuardReader {
public:
    /// The guard takes of every registered manager since the previous call (the first call: since the start).
    [[nodiscard]] u64 ReadDelta(const DirtyGuardRegistry& registry) {
        std::vector<Seen> now;
        u64 total = 0;
        registry.ForEach([&](u64 serial, size_t index, const GPUDirtyMemoryManager& manager) {
            const u64 cumulative = manager.GuardTakes();
            u64 previous = 0;
            for (const Seen& seen : seen_) {
                if (seen.serial == serial && seen.index == index) {
                    previous = seen.cumulative;
                    break;
                }
            }
            total += cumulative - previous; // monotonic: the manager only ever adds
            now.push_back({serial, index, cumulative});
        });
        seen_ = std::move(now);
        return total;
    }

private:
    struct Seen {
        u64 serial;
        size_t index;
        u64 cumulative;
    };
    std::vector<Seen> seen_;
};

} // namespace Core
