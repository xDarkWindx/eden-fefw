// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The notice ledger of docs/list-diag-design.md, 3.4: slots with a generation and a phase, an
// all-atomic payload, one `marks` word that carries the label of the generation, bounded retries
// and no waits. Header-only and self-contained: the emulator and tools/list-diag-harness include
// the same file.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "video_core/list_diag/counters.h"

namespace VideoCommon::ListDiag {

constexpr u64 kFree = 0;
constexpr u64 kWriting = 1;
constexpr u64 kPublished = 2;
constexpr unsigned kScan = 8; ///< Slots a writer examines before it drops the notice

// marks: tag (16) | popped | applied | checked | late | t_pop (22) | t_apply (22)
constexpr u64 kBitPopped = u64{1} << 16;
constexpr u64 kBitApplied = u64{1} << 17;
constexpr u64 kBitChecked = u64{1} << 18;
constexpr u64 kBitLate = u64{1} << 19;
constexpr unsigned kPopShift = 20;
constexpr unsigned kApplyShift = 42;
constexpr u64 kTimeMax = (u64{1} << 22) - 1;

/// The label of a generation: 16 bits and never zero (zero means "no payload" in the pop words and
/// "never published" in `marks`). The generation itself has 62 bits and is what `state`, `Handle`
/// and `Free` use; only `marks` and the pop payload carry the label, and only the operations that
/// compare a label take it from a generation. The label repeats every 65 535 generations of one
/// slot (the ABA bound of the design, 3.4).
constexpr u64 Tag(u64 generation) {
    return generation % 65535 + 1;
}

/// Coverage floor (docs/list-diag-design.md, 3.7.3): every lot of this core up to `h` has incomplete
/// coverage. One writer per core (pi4), so a load and a store; monotonic. seq_cst: the applier reads
/// it after its sweep and the argument rests on the total order.
inline void RaiseIncomplete(WriterShared& shared, WriterCounters& counters, u64 h) {
    if (shared.incomplete_floor.load(std::memory_order_seq_cst) < h) {
        shared.incomplete_floor.store(h, std::memory_order_seq_cst);
        counters.Bump(counters.floor_raises);
    }
}

enum class MarkKind { Popped, Applied, Checked, Late };
enum class MarkResult { Ok, Already, Lost };

struct alignas(64) Slot {
    std::atomic<u64> state{};   ///< generation (62) | phase (2)
    std::atomic<u64> marks{};
    std::atomic<u64> addr{};
    std::atomic<u64> meta{};    ///< size | core << 32 | ambiguous << 40
    std::atomic<u64> lot{};
    std::atomic<u64> t_write{}; ///< microseconds
    std::atomic<u64> pop1{};    ///< tag (16) | sample start (40) | sample length (8)
    std::atomic<u64> pop2{};    ///< tag (16) | hash (48)
};
static_assert(sizeof(Slot) == 64);

inline u64 Fnv48(const u8* data, size_t len) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h & ((u64{1} << 48) - 1);
}

struct View {
    u64 gen = 0; ///< The full generation
    u64 addr = 0;
    u32 size = 0;
    u32 core = 0;
    bool ambiguous = false;
    u64 lot = 0;
    u64 t_write = 0;
    Slot* slot = nullptr;
    u64 tag() const {
        return Tag(gen);
    }
};

struct Handle {
    Slot* slot = nullptr;
    u64 gen = 0; ///< The full generation (never truncated: Free compares it with `state`)
    bool ok = false;
    u64 tag() const {
        return Tag(gen);
    }
};

/// A record that was still published and not finished when its slot was reused.
struct Reclaimed {
    bool happened = false;     ///< The slot held an expired, unfinished record
    bool was_popped = false;   ///< ... that a pop had reached
    u64 addr = 0;
    u32 size = 0;
    u32 core = 0;
    u64 lot = 0;
};

template <class Hooks>
class Ledger {
public:
    Ledger(unsigned cores, unsigned slots_per_core, Counters& counters)
        : counters_{counters}, cores_{cores}, per_core_{slots_per_core} {
        const unsigned words = (slots_per_core + 63) / 64;
        for (unsigned c = 0; c < cores; ++c) {
            rings_.emplace_back(new Slot[slots_per_core]);
            live_.emplace_back(new std::atomic<u64>[words]());
        }
        cursor_.assign(cores, 0);
    }

    u64 expiry_us = 5'000'000;

    unsigned Cores() const {
        return cores_;
    }
    unsigned SlotsPerCore() const {
        return per_core_;
    }
    Slot& SlotAt(unsigned core, unsigned index) {
        return rings_[core][index];
    }

    /// Visits the slots that may hold a published record: the cost of a sweep is the number of live
    /// records, not the size of the ring (the pop and the application sweep once per range, under
    /// the buffer cache mutex). The index is a hint: the caller still reads `state` of each slot.
    /// Order: the writer publishes `state` and THEN sets the bit, both seq_cst, so a sweep that comes
    /// after the writer's read of apply_started_lot sees the bit (docs/list-diag-design.md, 3.3.1).
    template <class F>
    void ForEachLive(F&& visit) {
        const unsigned words = (per_core_ + 63) / 64;
        for (unsigned core = 0; core < cores_; ++core) {
            for (unsigned w = 0; w < words; ++w) {
                u64 bits = live_[core][w].load(std::memory_order_seq_cst);
                while (bits != 0) {
                    const unsigned bit = static_cast<unsigned>(std::countr_zero(bits));
                    bits &= bits - 1;
                    visit(core, w * 64 + bit);
                }
            }
        }
    }

    /// Slots whose bit is set (tests: at rest it must equal the published slots).
    u64 LiveBits() const {
        u64 n = 0;
        const unsigned words = (per_core_ + 63) / 64;
        for (unsigned core = 0; core < cores_; ++core) {
            for (unsigned w = 0; w < words; ++w) {
                n += static_cast<u64>(std::popcount(live_[core][w].load(std::memory_order_seq_cst)));
            }
        }
        return n;
    }

    /// Writer side (one writer per core). Bounded: it never waits.
    /// `now` is the ONE clock read of the notice (docs/list-diag-design.md, 3.7.5): the expiry test of
    /// every slot examined and the time of the record. `lot` of an ambiguous record is its ceiling `hi`.
    /// Before a record that is published, expired and NOT finished is recycled, the coverage floor
    /// rises to its lot (3.7.3, step 5); an overflow is declared by the caller, which knows `hi`.
    Handle Register(unsigned core, u64 addr, u32 size, u64 lot, bool ambiguous, u64 now, Reclaimed* reclaimed,
                    WriterCounters& wc, WriterShared& ws) {
        Slot* ring = rings_[core].get();
        for (unsigned tries = 0; tries < kScan && tries < per_core_; ++tries) {
            const unsigned index = (cursor_[core] + tries) % per_core_;
            Slot& s = ring[index];
            u64 st = s.state.load(std::memory_order_seq_cst);
            const u64 phase = st & 3;
            const u64 gen = st >> 2;
            bool reclaim = false;
            if (phase == kPublished && Expired(s, now)) {
                reclaim = true;
            } else if (phase != kFree) {
                continue;
            }
            const u64 old_addr = s.addr.load(std::memory_order_relaxed);
            const u64 old_meta = s.meta.load(std::memory_order_relaxed);
            const u64 old_lot = s.lot.load(std::memory_order_relaxed);
            const u64 next_gen = gen + 1;
            bool skip_coverage = false;
            if constexpr (Hooks::kTest) {
                skip_coverage = Hooks::NaiveNoCoverage();
            }
            if (reclaim && !skip_coverage &&
                (s.marks.load(std::memory_order_seq_cst) & (kBitApplied | kBitLate)) == 0) {
                // Not finished: its notice may be lost to the applier from here on. Raise BEFORE the
                // CAS, so a sweep that sees the new state also sees the floor.
                RaiseIncomplete(ws, wc, old_lot);
            }
            Hooks::At("r.before_reclaim_cas");
            if (!s.state.compare_exchange_strong(st, (next_gen << 2) | kWriting,
                                                 std::memory_order_seq_cst)) {
                continue;
            }
            s.addr.store(addr, std::memory_order_relaxed);
            s.meta.store(size | (u64{core} << 32) | (u64{ambiguous} << 40), std::memory_order_relaxed);
            s.lot.store(lot, std::memory_order_relaxed);
            s.t_write.store(now, std::memory_order_relaxed);
            s.pop1.store(0, std::memory_order_relaxed);
            s.pop2.store(0, std::memory_order_relaxed);
            // Seals the old marks: a reader that marks later fails on the label, and the value
            // taken here says whether the old record had already ended (applied or late). Only a
            // record that had not ended counts as expired; counting it from the state alone would
            // count a retired record twice (the retirement and the reuse).
            const u64 old_marks = s.marks.exchange(Tag(next_gen), std::memory_order_seq_cst);
            if (reclaim && (old_marks & (kBitApplied | kBitLate)) == 0) {
                wc.Bump(wc.expired);
                if (reclaimed != nullptr) {
                    reclaimed->happened = true;
                    reclaimed->was_popped = (old_marks & kBitPopped) != 0;
                    reclaimed->addr = old_addr;
                    reclaimed->size = static_cast<u32>(old_meta & 0xffffffffu);
                    reclaimed->core = static_cast<u32>((old_meta >> 32) & 0xff);
                    reclaimed->lot = old_lot;
                }
            }
            s.state.store((next_gen << 2) | kPublished, std::memory_order_seq_cst);
            SetLive(core, index);
            cursor_[core] = (index + 1) % per_core_;
            return Handle{&s, next_gen, true};
        }
        // The cursor only moved on success, so after an overflow the next notice would examine the SAME
        // slots and overflow again even with the ring nearly empty (docs/list-diag-design.md, 8.5 and
        // T3a.1b). Skip the kScan slots just examined. The cursor is private to the core's writer (pi4):
        // not part of the proof, nothing else changes.
        cursor_[core] = (cursor_[core] + kScan) % per_core_;
        wc.Bump(wc.overflow);
        return Handle{};
    }

    /// Reader side: a consistent copy of the payload, or false (and a lost observation when the
    /// slot was reused while reading).
    bool Read(Slot& s, View& v) {
        const u64 st = s.state.load(std::memory_order_seq_cst);
        if ((st & 3) != kPublished) {
            return false;
        }
        v.gen = st >> 2;
        v.slot = &s;
        v.addr = s.addr.load(std::memory_order_relaxed);
        const u64 meta = s.meta.load(std::memory_order_relaxed);
        v.size = static_cast<u32>(meta & 0xffffffffu);
        v.core = static_cast<u32>((meta >> 32) & 0xff);
        v.ambiguous = ((meta >> 40) & 1) != 0;
        v.lot = s.lot.load(std::memory_order_relaxed);
        v.t_write = s.t_write.load(std::memory_order_relaxed);
        Hooks::At("r.after_payload_read");
        Hooks::At("r.between_state_loads");
#if defined(__SANITIZE_THREAD__)
        // ThreadSanitizer does not model fences (and GCC refuses them with -Werror=tsan): an
        // acq_rel read-modify-write of +0 gives the same ordering and is understood by it.
        s.state.fetch_add(0, std::memory_order_acq_rel);
#else
        std::atomic_thread_fence(std::memory_order_acquire);
#endif
        if (s.state.load(std::memory_order_seq_cst) != st) {
            Inc(counters_.observation_lost);
            return false;
        }
        return true;
    }

    /// Sets a mark by CAS on `marks`, expecting the label that was read. At most two attempts;
    /// contention or a reused slot is a lost observation, never a wait.
    MarkResult Mark(Slot& s, u64 tag, MarkKind kind, u64 t_write) {
        Hooks::At(kind == MarkKind::Popped    ? "m.before_pop_cas"
                  : kind == MarkKind::Applied ? "m.before_apply_cas"
                                              : "m.before_check_cas");
        // Only the marks that record a width read the clock; Checked and Late (the writer's) do not.
        u64 dt = 0;
        if (kind == MarkKind::Popped || kind == MarkKind::Applied) {
            const u64 now = Hooks::NowUs();
            dt = std::min<u64>(now >= t_write ? (now - t_write) / 2 : 0, kTimeMax);
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            u64 m = s.marks.load(std::memory_order_seq_cst);
            if ((m & 0xffff) != tag) {
                Inc(counters_.observation_lost);
                return MarkResult::Lost;
            }
            u64 add = 0;
            switch (kind) {
            case MarkKind::Popped:
                if ((m & kBitPopped) != 0) return MarkResult::Already;
                add = kBitPopped | (dt << kPopShift);
                break;
            case MarkKind::Applied:
                if ((m & (kBitApplied | kBitLate)) != 0) return MarkResult::Already;
                add = kBitApplied | (dt << kApplyShift);
                break;
            case MarkKind::Checked:
                if ((m & (kBitChecked | kBitApplied | kBitLate)) != 0) return MarkResult::Already;
                add = kBitChecked;
                break;
            case MarkKind::Late:
                if ((m & (kBitApplied | kBitLate)) != 0) return MarkResult::Already;
                add = kBitLate | kBitChecked;
                break;
            }
            if (s.marks.compare_exchange_strong(m, m | add, std::memory_order_seq_cst)) {
                return MarkResult::Ok;
            }
        }
        Inc(counters_.observation_lost);
        return MarkResult::Lost;
    }

    u64 Marks(Slot& s) const {
        return s.marks.load(std::memory_order_seq_cst);
    }

    /// published -> free, same (full) generation. One attempt: failure means somebody else moved it.
    void Free(unsigned core, Slot& s, u64 gen) {
        u64 expected = (gen << 2) | kPublished;
        if (s.state.compare_exchange_strong(expected, (gen << 2) | kFree, std::memory_order_seq_cst)) {
            ClearLive(core, static_cast<unsigned>(&s - rings_[core].get()));
        }
    }

    // The pop payload carries the label in each word, so that a late write by a reader of an older
    // generation can never be taken for a payload of the current one.
    static u64 PackPop1(u64 tag, u64 start, u64 len) {
        return (tag << 48) | ((start & ((u64{1} << 40) - 1)) << 8) | (len & 0xff);
    }
    static u64 PackPop2(u64 tag, u64 hash) {
        return (tag << 48) | (hash & ((u64{1} << 48) - 1));
    }
    void WritePop(Slot& s, u64 tag, u64 start, u64 len, u64 hash) {
        Hooks::At("m.before_pop_payload");
        s.pop1.store(PackPop1(tag, start, len), std::memory_order_relaxed);
        s.pop2.store(PackPop2(tag, hash), std::memory_order_relaxed);
    }
    struct PopPayload {
        bool valid = false;
        u64 start = 0, len = 0, hash = 0;
    };
    PopPayload ReadPop(Slot& s, u64 tag) const {
        const u64 p1 = s.pop1.load(std::memory_order_relaxed);
        const u64 p2 = s.pop2.load(std::memory_order_relaxed);
        PopPayload r;
        if ((p1 >> 48) == tag && (p2 >> 48) == tag && tag != 0) {
            r.valid = true;
            r.start = (p1 >> 8) & ((u64{1} << 40) - 1);
            r.len = p1 & 0xff;
            r.hash = p2 & ((u64{1} << 48) - 1);
        }
        return r;
    }

    // White box, for the tests only.
    /// The next claim of this slot will have generation `next_claim_gen`: lets a test cross the
    /// 16-bit boundary of the labels without 65 536 real reuses.
    void TestSetGeneration(unsigned core, unsigned index, u64 next_claim_gen) {
        Slot& s = rings_[core][index];
        s.state.store(((next_claim_gen - 1) << 2) | kFree, std::memory_order_seq_cst);
        s.marks.store(0, std::memory_order_seq_cst);
    }
    void TestWritePopWords(Slot& s, u64 tag, u64 start, u64 len, u64 hash) {
        s.pop1.store(PackPop1(tag, start, len), std::memory_order_relaxed);
        s.pop2.store(PackPop2(tag, hash), std::memory_order_relaxed);
    }

private:
    void SetLive(unsigned core, unsigned index) {
        live_[core][index / 64].fetch_or(u64{1} << (index % 64), std::memory_order_seq_cst);
    }

    /// Clears the bit and then looks again: a writer may have reused the slot between the Free and
    /// the clear (its own set would be lost), in which case the bit goes back.
    void ClearLive(unsigned core, unsigned index) {
        Hooks::At("l.before_clear");
        live_[core][index / 64].fetch_and(~(u64{1} << (index % 64)), std::memory_order_seq_cst);
        Hooks::At("l.after_clear");
        if ((rings_[core][index].state.load(std::memory_order_seq_cst) & 3) == kPublished) {
            SetLive(core, index);
        }
    }

    bool Expired(Slot& s, u64 now) const {
        const u64 t = s.t_write.load(std::memory_order_relaxed);
        return now > t && now - t > expiry_us;
    }

    Counters& counters_;
    unsigned cores_;
    unsigned per_core_;
    std::vector<std::unique_ptr<Slot[]>> rings_;
    std::vector<std::unique_ptr<std::atomic<u64>[]>> live_; ///< Index of published slots, one bit each
    std::vector<unsigned> cursor_;
};

} // namespace VideoCommon::ListDiag
