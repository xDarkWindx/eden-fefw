// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Model of the notice ledger of docs/list-diag-design.md 3.4: slots with a generation and a
// phase, an all-atomic payload, one `marks` word that carries the generation, bounded retries.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "gates.h"

namespace diag {

constexpr u64 kFree = 0;
constexpr u64 kWriting = 1;
constexpr u64 kPublished = 2;
constexpr unsigned kScan = 8; // slots a writer examines before dropping the notice

// marks: generation (16) | popped | applied | checked | late | t_pop (22) | t_apply (22)
constexpr u64 kBitPopped = u64{1} << 16;
constexpr u64 kBitApplied = u64{1} << 17;
constexpr u64 kBitChecked = u64{1} << 18;
constexpr u64 kBitLate = u64{1} << 19;
constexpr unsigned kPopShift = 20;
constexpr unsigned kApplyShift = 42;
constexpr u64 kTimeMax = (u64{1} << 22) - 1;

enum class MarkKind { Popped, Applied, Checked, Late };
enum class MarkResult { Ok, Already, Lost };

struct alignas(64) Slot {
    std::atomic<u64> state{};   // generation (62) | phase (2)
    std::atomic<u64> marks{};
    std::atomic<u64> addr{};
    std::atomic<u64> meta{};    // size | core << 32 | ambiguous << 40
    std::atomic<u64> lot{};
    std::atomic<u64> t_write{}; // microseconds
    std::atomic<u64> pop1{};    // tag (16) | sample start (40) | sample length (8)
    std::atomic<u64> pop2{};    // tag (16) | hash (48)
};
static_assert(sizeof(Slot) == 64);

struct Counters {
    std::atomic<u64> notice_registered{}, notice_applied{}, notice_expired{}, notice_overflow{},
        notice_ambiguous{}, notice_late{};
    std::atomic<u64> pop_ranges{}, pop_over_notice{}, pop_over_notice_bytes{},
        pop_over_notice_records{}, pop_over_ambiguous{}, pop_over_unchecked{};
    std::atomic<u64> window_sample_equal{}, window_sample_different{}, window_unverified{};
    std::atomic<u64> apply_overlap_other_lot{}, apply_overlap_ambiguous_lot{}, apply_partial{},
        apply_out_of_order{};
    std::atomic<u64> observation_lost{};
    std::atomic<u64> invalidate_calls{}, gather_overlapped{};
};

inline void Inc(std::atomic<u64>& a, u64 n = 1) {
    a.fetch_add(n, std::memory_order_relaxed);
}
inline u64 Get(const std::atomic<u64>& a) {
    return a.load(std::memory_order_relaxed);
}

inline u64 Fnv48(const u8* data, size_t len) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h & ((u64{1} << 48) - 1);
}

struct View {
    u64 gen = 0;
    u64 addr = 0;
    u32 size = 0;
    u32 core = 0;
    bool ambiguous = false;
    u64 lot = 0;
    u64 t_write = 0;
    Slot* slot = nullptr;
    u64 gen16() const {
        return gen & 0xffff;
    }
};

struct Handle {
    Slot* slot = nullptr;
    u64 gen16 = 0;
    bool ok = false;
};

class Ledger {
public:
    Ledger(unsigned cores, unsigned slots_per_core, Counters& counters, const std::atomic<u64>& clock)
        : counters_{counters}, clock_{clock}, cores_{cores}, per_core_{slots_per_core} {
        for (unsigned c = 0; c < cores; ++c) {
            rings_.emplace_back(new Slot[slots_per_core]);
        }
        cursor_.assign(cores, 0);
    }

    Gates* gates = nullptr;
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

    /// Writer side (one writer per core). Bounded: never waits.
    Handle Register(unsigned core, u64 addr, u32 size, u64 lot, bool ambiguous) {
        Slot* ring = rings_[core].get();
        for (unsigned tries = 0; tries < kScan && tries < per_core_; ++tries) {
            const unsigned index = (cursor_[core] + tries) % per_core_;
            Slot& s = ring[index];
            u64 st = s.state.load(std::memory_order_seq_cst);
            const u64 phase = st & 3;
            const u64 gen = st >> 2;
            bool reclaim = false;
            if (phase == kPublished && Expired(s)) {
                reclaim = true;
            } else if (phase != kFree) {
                continue;
            }
            const u64 next_gen = gen + 1;
            if (!s.state.compare_exchange_strong(st, (next_gen << 2) | kWriting,
                                                 std::memory_order_seq_cst)) {
                continue;
            }
            s.addr.store(addr, std::memory_order_relaxed);
            s.meta.store(size | (u64{core} << 32) | (u64{ambiguous} << 40),
                         std::memory_order_relaxed);
            s.lot.store(lot, std::memory_order_relaxed);
            s.t_write.store(clock_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            s.pop1.store(0, std::memory_order_relaxed);
            s.pop2.store(0, std::memory_order_relaxed);
            // Seals the old marks: a reader that marks later fails on the generation, and the
            // value taken here says whether the old record had already ended (applied or late).
            // Only a record that had not ended counts as expired; counting it from the state
            // alone would count a retired record twice (the retirement and the reuse).
            const u64 old_marks = s.marks.exchange(next_gen & 0xffff, std::memory_order_seq_cst);
            if (reclaim && (old_marks & (kBitApplied | kBitLate)) == 0) {
                Inc(counters_.notice_expired);
            }
            s.state.store((next_gen << 2) | kPublished, std::memory_order_seq_cst);
            cursor_[core] = (index + 1) % per_core_;
            return Handle{&s, next_gen & 0xffff, true};
        }
        Inc(counters_.notice_overflow);
        return Handle{};
    }

    /// Reader side: a consistent copy of the payload, or false (and a lost observation when
    /// the slot was recycled while reading).
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
        Pass(gates, "r.after_payload_read");
#if defined(__SANITIZE_THREAD__)
        // ThreadSanitizer does not model fences (and GCC refuses them with -Werror=tsan): an
        // acq_rel read-modify-write of +0 gives the same ordering and is understood by it. The
        // -O2 and ASan builds use the fence of the design (docs/list-diag-design.md 3.4).
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

    /// Sets a mark by CAS on `marks`, expecting the generation that was read. At most two
    /// attempts; contention or a recycled slot is a lost observation, never a wait.
    MarkResult Mark(Slot& s, u64 gen16, MarkKind kind, u64 t_write) {
        const char* gate = kind == MarkKind::Popped    ? "m.before_pop_cas"
                           : kind == MarkKind::Applied ? "m.before_apply_cas"
                                                       : "m.before_check_cas";
        Pass(gates, gate);
        const u64 now = clock_.load(std::memory_order_relaxed);
        const u64 dt = std::min<u64>(now >= t_write ? (now - t_write) / 2 : 0, kTimeMax);
        for (int attempt = 0; attempt < 2; ++attempt) {
            u64 m = s.marks.load(std::memory_order_seq_cst);
            if ((m & 0xffff) != gen16) {
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

    /// published -> free, same generation. One attempt: failure means somebody else moved it.
    void Free(Slot& s, u64 gen) {
        u64 expected = (gen << 2) | kPublished;
        s.state.compare_exchange_strong(expected, (gen << 2) | kFree, std::memory_order_seq_cst);
    }

    // The pop payload carries the generation in each word, so that a late write by a reader of
    // an older generation can never be taken for a payload of the current one.
    static u64 PackPop1(u64 gen16, u64 start, u64 len) {
        return (gen16 << 48) | ((start & ((u64{1} << 40) - 1)) << 8) | (len & 0xff);
    }
    static u64 PackPop2(u64 gen16, u64 hash) {
        return (gen16 << 48) | (hash & ((u64{1} << 48) - 1));
    }
    void WritePop(Slot& s, u64 gen16, u64 start, u64 len, u64 hash) {
        Pass(gates, "m.before_pop_payload");
        s.pop1.store(PackPop1(gen16, start, len), std::memory_order_relaxed);
        s.pop2.store(PackPop2(gen16, hash), std::memory_order_relaxed);
    }
    struct PopPayload {
        bool valid = false;
        u64 start = 0, len = 0, hash = 0;
    };
    PopPayload ReadPop(Slot& s, u64 gen16) const {
        const u64 p1 = s.pop1.load(std::memory_order_relaxed);
        const u64 p2 = s.pop2.load(std::memory_order_relaxed);
        PopPayload r;
        if ((p1 >> 48) == gen16 && (p2 >> 48) == gen16 && gen16 != 0) {
            r.valid = true;
            r.start = (p1 >> 8) & ((u64{1} << 40) - 1);
            r.len = p1 & 0xff;
            r.hash = p2 & ((u64{1} << 48) - 1);
        }
        return r;
    }
    /// White box, for the stale payload test only.
    void TestWritePopWords(Slot& s, u64 gen16, u64 start, u64 len, u64 hash) {
        s.pop1.store(PackPop1(gen16, start, len), std::memory_order_relaxed);
        s.pop2.store(PackPop2(gen16, hash), std::memory_order_relaxed);
    }

private:
    bool Expired(Slot& s) const {
        const u64 now = clock_.load(std::memory_order_relaxed);
        const u64 t = s.t_write.load(std::memory_order_relaxed);
        return now > t && now - t > expiry_us;
    }

    Counters& counters_;
    const std::atomic<u64>& clock_;
    unsigned cores_;
    unsigned per_core_;
    std::vector<std::unique_ptr<Slot[]>> rings_;
    std::vector<unsigned> cursor_;
};

} // namespace diag
