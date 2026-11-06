// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Integration tests of the EDEN_LIST_DIAG diagnostic (docs/list-diag-design.md, 8.1), parts pop
// and blk. They run the SAME protocol the emulator runs (video_core/list_diag/protocol.h, a
// header-only file) against the REAL Core::GPUDirtyMemoryManager and the real Common::RangeSet;
// only the buffer cache around them is replaced by what the hooks need (the cache mutex, the
// ranges, a guest read). tools/list-diag-harness/run.sh compiles this same file with TSan and
// ASan. Every interleaving is chosen with named stop points (`Gates`), not hoped for.
//
// Assertions are made on the main thread only; the threads of a test just do work.

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "core/gpu_dirty_memory_manager.h"
#include "video_core/list_diag.h"
#include "video_core/list_diag/protocol.h"

#include "list_diag_legacy_manager.h"

namespace {

using namespace VideoCommon::ListDiag;
using Manager = Core::GPUDirtyMemoryManager;

constexpr unsigned kCores = 3;

// ---------------------------------------------------------------------------------------------
// Stop points: a thread that reaches an armed gate blocks until the test releases it.

class Gates {
public:
    void Arm(const std::string& name) {
        std::lock_guard lock{m_};
        armed_.insert(name);
    }
    void Pass(const char* name) {
        std::unique_lock lock{m_};
        if (armed_.count(name) == 0 || released_.count(name) != 0) {
            return;
        }
        reached_.insert(name);
        cv_.notify_all();
        if (!cv_.wait_for(lock, std::chrono::seconds{20}, [&] { return released_.count(name) != 0; })) {
            std::fprintf(stderr, "gate %s was never released\n", name);
            std::abort();
        }
    }
    void WaitReached(const std::string& name) {
        std::unique_lock lock{m_};
        if (!cv_.wait_for(lock, std::chrono::seconds{20}, [&] { return reached_.count(name) != 0; })) {
            std::fprintf(stderr, "gate %s was never reached\n", name.c_str());
            std::abort();
        }
    }
    void Release(const std::string& name) {
        std::lock_guard lock{m_};
        released_.insert(name);
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::set<std::string> armed_, reached_, released_;
};

inline Gates* g_gates = nullptr;
inline std::atomic<u64> g_clock{0};
inline std::atomic<bool> g_skip_overlap{false};
inline std::atomic<long long> g_mutant{0};
inline std::atomic<bool> g_naive{false};
inline std::function<void(u64, u32)> g_on_window_flag;

/// The hooks of the tests: the same members as ProdHooks, with `kTest` on.
struct TestHooks {
    static constexpr bool kTest = true;
    static void At(const char* name) {
        if (g_gates != nullptr) {
            g_gates->Pass(name);
        }
    }
    static bool SkipGatherOnOverlap() {
        return g_skip_overlap.load();
    }
    static long long MutantDelta() {
        return g_mutant.load();
    }
    static bool NaiveNoLateCheck() {
        return g_naive.load();
    }
    static void OnWindowFlag(u64 addr, u32 size) {
        if (g_on_window_flag) {
            g_on_window_flag(addr, size);
        }
    }
    static u64 NowUs() {
        return g_clock.load(std::memory_order_relaxed);
    }
};

using Proto = Protocol<kCores, TestHooks>;

constexpr u8 kBytes[64] = {1, 2, 3, 4};
constexpr u8 kGuestFill = 0xEE;

/// The scaffolding the emulator has around the hooks: the join of ranges before OnCachedWrites
/// (gpu.cpp:143-161, unchanged there), the application under the cache mutex and the pop under it.
struct Fx {
    explicit Fx(unsigned slots = 64) : proto(slots) {
        g_clock = 0;
        g_skip_overlap = false;
        g_mutant = 0;
        g_naive = false;
        g_on_window_flag = nullptr;
        g_gates = &gates;
        read_guest = [](u64, u8* out, size_t len) { std::memset(out, kGuestFill, len); };
    }
    ~Fx() {
        g_gates = nullptr;
        g_on_window_flag = nullptr;
        g_skip_overlap = false;
        g_mutant = 0;
        g_naive = false;
    }

    std::array<Manager, kCores> managers;
    Proto proto;
    Gates gates;
    std::mutex cache_mutex; ///< buffer_cache.mutex
    Common::RangeSet<u64> gpu_modified;
    Common::RangeSet<u64> cpu_read;
    bool track_cpu_reads = false;
    std::function<void(u64, u8*, size_t)> read_guest;
    std::function<void(u64, u64)> on_range_applied;

    Counters& c() {
        return proto.counters;
    }
    u64 C(const Counter& counter) const {
        return Get(counter);
    }

    Proto::Claim Collect(unsigned core, u64 addr, u32 size = 4) {
        return proto.CollectObserved(managers[core], core, addr, size);
    }

    Proto::Batch GatherAll() {
        Proto::Gathered gathered;
        proto.GatherAll(std::span<Manager>{managers}, gathered);
        std::vector<std::pair<u64, u64>> ranges;
        for (const Range& r : gathered.pre) {
            ranges.emplace_back(r.addr, r.size);
        }
        if (ranges.size() > 1) {
            std::sort(ranges.begin(), ranges.end());
            size_t out = 0;
            for (size_t in = 1; in < ranges.size(); ++in) {
                auto& last = ranges[out];
                const auto& [address, size] = ranges[in];
                const u64 last_end = last.first + last.second;
                if ((address >> 12) == (last.first >> 12) && address <= last_end) {
                    last.second = std::max(last_end, address + size) - last.first;
                } else {
                    ranges[++out] = ranges[in];
                }
            }
            ranges.resize(out + 1);
        }
        return proto.MakeBatch(std::move(gathered), ranges);
    }

    void ApplyBatchRanges(const Proto::Batch& batch) {
        proto.BeginApply(batch);
        for (const Range& r : batch.merged) {
            std::lock_guard lock{cache_mutex};
            proto.NoteApply(r.addr, r.size, gpu_modified, cpu_read, track_cpu_reads, read_guest);
            if (on_range_applied) {
                on_range_applied(r.addr, r.size);
            }
        }
        proto.EndApply();
    }

    void Invalidate() {
        const Proto::Batch batch = GatherAll();
        ApplyBatchRanges(batch);
    }

    void Pop(u64 start, u64 end, const u8* bytes = kBytes) {
        std::lock_guard lock{cache_mutex};
        proto.NotePop(start, end, bytes);
    }

    size_t PublishedSlots() {
        size_t n = 0;
        for (unsigned core = 0; core < kCores; ++core) {
            for (unsigned i = 0; i < proto.ledger.SlotsPerCore(); ++i) {
                if ((proto.ledger.SlotAt(core, i).state.load() & 3) == kPublished) {
                    ++n;
                }
            }
        }
        return n;
    }

    bool Conserved() {
        // At rest the index of live slots is exactly the set of published slots.
        if (proto.ledger.LiveBits() != PublishedSlots()) {
            return false;
        }
        return Get(c().notice_registered) ==
               Get(c().notice_applied) + Get(c().notice_late) + Get(c().notice_expired) + PublishedSlots();
    }

    std::vector<std::string> Lines() {
        std::vector<std::string> lines;
        proto.Drain([&](const std::string& line) { lines.push_back(line); });
        return lines;
    }
};

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

} // namespace

// =============================================================================================
// The manager: the diagnostic does not change what the real manager delivers.

TEST_CASE("list diag: Collect and Gather, Diag=false and Diag=true, equal the manager of before",
          "[list-diag]") {
    Legacy::GPUDirtyMemoryManager legacy;
    Manager plain;
    Manager diagnostic;
    Core::GatherLotState lots;
    std::mt19937 rng{1234};
    using Out = std::vector<std::pair<u64, u64>>;
    bool all_equal = true;
    for (int step = 0; step < 40000; ++step) {
        if (rng() % 7 == 0) {
            Out a, b, c;
            std::function<void(PAddr, size_t)> ca = [&](PAddr x, size_t y) { a.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cb = [&](PAddr x, size_t y) { b.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cc = [&](PAddr x, size_t y) { c.emplace_back(x, y); };
            legacy.Gather(ca);
            plain.Gather(cb);
            u64 lot = 0;
            diagnostic.GatherT<true, TestHooks>(cc, &lots, &lot);
            all_equal = all_equal && a == b && a == c;
        } else {
            const u64 page = rng() % 6;
            const u64 half = rng() % 2;
            const u64 off = (rng() % 480) * 4; // [off, off + size) stays inside one 2 KiB region
            const u64 size = 1 + rng() % std::min<u64>(96, 2048 - off);
            const u64 addr = 0x10000 + page * 4096 + half * 2048 + off;
            legacy.Collect(addr, size);
            plain.Collect(addr, size);
            diagnostic.CollectT<true, TestHooks>(addr, size);
        }
    }
    CHECK(all_equal);
    CHECK(lots.lot_started.load() == lots.lot_done.load());
    CHECK(lots.gather_active.load() == 0);
    CHECK(lots.gather_overlapped.load() == 0);
}

TEST_CASE("list diag: the lot state lives outside the manager, one cache line per core", "[list-diag]") {
    STATIC_REQUIRE(sizeof(Core::GatherLotState) == 64);
    STATIC_REQUIRE(alignof(Core::GatherLotState) == 64);
    // The manager of today has no member of the diagnostic: its layout is the one it always had.
    STATIC_REQUIRE(sizeof(Manager) == sizeof(Legacy::GPUDirtyMemoryManager));
}

// =============================================================================================
// The lot of a notice, by the identity of each notice.

namespace {

struct Step {
    bool gather;      // true: a Gather and its application; false: a notice
    u64 addr;
    u64 expected_lot; // scripted by construction: the number of Gathers before it, plus 1
};

struct ClaimsResult {
    unsigned identity_failures = 0;
    unsigned address_only_passes = 0;
    unsigned notices = 0;
};

ClaimsResult RunScript(const std::vector<Step>& steps, long long mutant) {
    Fx f;
    g_mutant = mutant;
    std::map<u64, std::set<u64>> blocks_of_lot; // the blocks each Gather delivered (core 0)
    std::vector<Proto::Claim> claims;
    for (const Step& s : steps) {
        if (s.gather) {
            const Proto::Batch b = f.GatherAll();
            for (const Range& r : b.pre) {
                if (r.mask != 1) continue;
                for (u64 a = r.addr; a < r.addr + r.size; a += 64) {
                    blocks_of_lot[b.lots[0]].insert(a);
                }
            }
            f.ApplyBatchRanges(b);
        } else {
            claims.push_back(f.Collect(0, s.addr));
        }
    }
    ClaimsResult res;
    size_t index = 0;
    for (const Step& s : steps) {
        if (s.gather) continue;
        const Proto::Claim& c = claims[index++];
        ++res.notices;
        if (c.ambiguous || c.lot != s.expected_lot) {
            ++res.identity_failures;
        }
        // The check that is not enough: "the claimed lot delivered this address".
        if (!c.ambiguous && blocks_of_lot[c.lot].count(s.addr & ~u64{63}) != 0) {
            ++res.address_only_passes;
        }
    }
    return res;
}

} // namespace

TEST_CASE("list diag: the lot of each notice, with the same 64-byte block in several lots", "[list-diag]") {
    // The same block in lots 1, 2 and 3 and twice in lot 4 (the second one is "already covered").
    const std::vector<Step> repeated{
        {false, 0x2000, 1}, {true, 0, 0},       {false, 0x2008, 2}, {true, 0, 0}, {false, 0x2000, 3},
        {true, 0, 0},       {false, 0x2004, 4}, {false, 0x2004, 4}, {true, 0, 0},
    };
    const ClaimsResult ok = RunScript(repeated, 0);
    CHECK(ok.notices == 5);
    CHECK(ok.identity_failures == 0);
    CHECK(ok.address_only_passes == 5);
    // A mutant that claims the next lot: the identity check sees every wrong claim, and the
    // address-only check is fooled whenever the block shows up in the wrong lot too.
    const ClaimsResult mutant = RunScript(repeated, 1);
    CHECK(mutant.identity_failures == 5);
    CHECK(mutant.address_only_passes == 3);

    // The three paths of Collect (core/gpu_dirty_memory_manager.h:26-47), all in lot 1: CAS into an
    // empty `current`, the guard path (another 2 KiB region), "already covered", the guard path again.
    const std::vector<Step> paths{
        {false, 0x3000, 1}, {false, 0x4000, 1}, {false, 0x4000, 1}, {false, 0x3000, 1}, {true, 0, 0},
    };
    const ClaimsResult p = RunScript(paths, 0);
    CHECK(p.notices == 4);
    CHECK(p.identity_failures == 0);
    CHECK(p.address_only_passes == 4);
}

TEST_CASE("list diag: a Gather around the registration makes the notice ambiguous, never guessed",
          "[list-diag]") {
    SECTION("a Gather incremented lot_started and has not taken the guard yet") {
        Fx f;
        f.gates.Arm("g.after_inc");
        Proto::Batch b;
        std::thread gt([&] { b = f.GatherAll(); });
        f.gates.WaitReached("g.after_inc");
        const auto c = f.Collect(0, 0x6000);
        CHECK(c.ambiguous);
        f.gates.Release("g.after_inc");
        gt.join();
        f.ApplyBatchRanges(b);
        CHECK(f.C(f.c().apply_overlap_ambiguous_lot) == 1);
        CHECK(f.C(f.c().notice_applied) == 0);
    }
    SECTION("the Gather takes the notice between the Collect and the second read") {
        Fx f;
        f.gates.Arm("w.after_collect");
        Proto::Claim c;
        std::thread wt([&] { c = f.Collect(0, 0x6100); });
        f.gates.WaitReached("w.after_collect");
        f.Invalidate();
        f.gates.Release("w.after_collect");
        wt.join();
        CHECK(c.ambiguous);
    }
    SECTION("a whole Gather between the first reads and the Collect") {
        Fx f;
        f.gates.Arm("w.after_r0");
        Proto::Claim c;
        std::thread wt([&] { c = f.Collect(0, 0x6200); });
        f.gates.WaitReached("w.after_r0");
        f.Invalidate();
        f.gates.Release("w.after_r0");
        wt.join();
        CHECK(c.ambiguous);
    }
    SECTION("lot_done written, the callbacks not run yet: the notice is of the next lot") {
        Fx f;
        f.gates.Arm("g.before_iter");
        Proto::Batch b;
        std::thread gt([&] { b = f.GatherAll(); });
        f.gates.WaitReached("g.before_iter");
        const auto c = f.Collect(0, 0x6300);
        CHECK(!c.ambiguous);
        CHECK(c.lot == 2);
        f.gates.Release("g.before_iter");
        gt.join();
        f.ApplyBatchRanges(b);
        f.Invalidate(); // lot 2 delivers the block and retires the record
        CHECK(f.C(f.c().notice_applied) == 1);
    }
}

TEST_CASE("list diag: the application of A does not retire B, registered later in the same block",
          "[list-diag]") {
    Fx f;
    const auto a = f.Collect(0, 0x7000);
    CHECK(a.lot == 1);
    const Proto::Batch b1 = f.GatherAll(); // takes A (lot 1); not applied yet
    const auto b = f.Collect(0, 0x7008);
    CHECK(!b.ambiguous);
    CHECK(b.lot == 2); // taken after lot 1 completed: the next lot
    f.ApplyBatchRanges(b1);
    CHECK(f.C(f.c().notice_applied) == 1);          // A, and only A
    CHECK(f.C(f.c().apply_overlap_other_lot) == 1); // B overlaps, another lot: counted, not retired
    f.Pop(0x7008, 0x700c);                          // B is still pending: the pop counts
    CHECK(f.C(f.c().pop_over_notice) == 1);
    f.Invalidate(); // lot 2: B retired now
    CHECK(f.C(f.c().notice_applied) == 2);
    CHECK(f.PublishedSlots() == 0);
}

TEST_CASE("list diag: a notice taken by the Gather and not applied is still pending in the ledger",
          "[list-diag]") {
    Fx f;
    f.Collect(0, 0xD000);
    const Proto::Batch b = f.GatherAll();
    f.Pop(0xD000, 0xD004);
    CHECK(f.C(f.c().pop_over_notice) == 1);
    const Proto::Batch again = f.GatherAll();
    CHECK(again.pre.empty()); // the manager no longer holds it: a peek would have missed it
    CHECK(f.C(f.c().gather_empty[0]) >= 1);
    f.ApplyBatchRanges(b);
    CHECK(f.C(f.c().notice_applied) == 1);
}

// =============================================================================================
// A notice published after the application of its lot.

TEST_CASE("list diag: a notice published after the application of its lot is never a pending window",
          "[list-diag]") {
    SECTION("paused between the second read and the publication; the whole application runs") {
        Fx f;
        f.gates.Arm("w.after_s2");
        Proto::Claim c;
        std::thread wt([&] { c = f.Collect(0, 0x8000); });
        f.gates.WaitReached("w.after_s2");
        f.Invalidate(); // takes lot 1 (the Collect is done), stores 1, sweeps: nothing
        f.gates.Release("w.after_s2");
        wt.join();
        CHECK((c.registered && !c.ambiguous));
        CHECK(c.lot == 1);
        CHECK(f.C(f.c().notice_late) == 1);
        CHECK(f.PublishedSlots() == 0);
        f.Pop(0x8000, 0x8004);
        CHECK(f.C(f.c().pop_over_notice) == 0); // no false window
        CHECK(f.C(f.c().pop_over_unchecked) == 0);
    }
    SECTION("the same without the check: the test is sensitive, the false window appears") {
        Fx f;
        g_naive = true;
        f.gates.Arm("w.after_s2");
        std::thread wt([&] { f.Collect(0, 0x8000); });
        f.gates.WaitReached("w.after_s2");
        f.Invalidate();
        f.gates.Release("w.after_s2");
        wt.join();
        f.Pop(0x8000, 0x8004);
        CHECK(f.C(f.c().pop_over_notice) == 1);
    }
    SECTION("paused between the publication and the read of apply_started_lot") {
        Fx f;
        f.gates.Arm("w.after_publish");
        std::thread wt([&] { f.Collect(0, 0x8100); });
        f.gates.WaitReached("w.after_publish");
        f.Pop(0x8100, 0x8104); // published, not checked: no claim
        CHECK(f.C(f.c().pop_over_unchecked) == 1);
        CHECK(f.C(f.c().pop_over_notice) == 0);
        f.Invalidate(); // the sweep sees the published record and retires it
        CHECK(f.C(f.c().notice_applied) == 1);
        f.gates.Release("w.after_publish");
        wt.join();
        CHECK(f.C(f.c().notice_late) == 0); // already retired: exactly one end state
        CHECK(f.C(f.c().notice_applied) == 1);
        CHECK(f.PublishedSlots() == 0);
    }
    SECTION("the application of lot 1 is running (lot stored, sweep not started) when it publishes") {
        Fx f;
        f.gates.Arm("w.after_s2");
        f.gates.Arm("a.after_astore");
        Proto::Claim c;
        std::thread wt([&] { c = f.Collect(0, 0x8200); });
        f.gates.WaitReached("w.after_s2");
        const Proto::Batch b = f.GatherAll();
        std::thread at([&] { f.ApplyBatchRanges(b); });
        f.gates.WaitReached("a.after_astore");
        f.gates.Release("w.after_s2"); // publishes, reads apply_started_lot == 1: late
        wt.join();
        f.gates.Release("a.after_astore"); // the sweep finds nothing
        at.join();
        CHECK(f.C(f.c().notice_late) == 1);
        CHECK(f.C(f.c().notice_applied) == 0);
        CHECK(f.PublishedSlots() == 0);
    }
    SECTION("published and checked before the application starts: a real window, then retired") {
        Fx f;
        f.Collect(0, 0x8300);
        f.Pop(0x8300, 0x8304);
        CHECK(f.C(f.c().pop_over_notice) == 1);
        f.Invalidate();
        CHECK(f.C(f.c().notice_applied) == 1);
        CHECK(f.C(f.c().notice_late) == 0);
    }
}

// =============================================================================================
// Ownership and generation of the slots.

TEST_CASE("list diag: the slot is reused while a reader reads, marks the pop, marks the apply",
          "[list-diag]") {
    SECTION("while the payload is read") {
        Fx f(1);
        f.proto.ledger.expiry_us = 100;
        f.Collect(0, 0x9000);
        f.gates.Arm("r.after_payload_read");
        std::thread pt([&] { f.Pop(0x9000, 0x9004); });
        f.gates.WaitReached("r.after_payload_read");
        g_clock += 200;
        f.Collect(0, 0x9100); // reclaims the expired slot
        CHECK(f.C(f.c().notice_expired) == 1);
        f.gates.Release("r.after_payload_read");
        pt.join();
        CHECK(f.C(f.c().observation_lost) >= 1);
        CHECK(f.C(f.c().pop_over_notice) == 0);
        f.Pop(0x9100, 0x9104); // the new record is intact
        CHECK(f.C(f.c().pop_over_notice) == 1);
    }
    SECTION("while the pop marks t_pop") {
        Fx f(1);
        f.proto.ledger.expiry_us = 100;
        f.Collect(0, 0x9200);
        f.gates.Arm("m.before_pop_cas");
        std::thread pt([&] { f.Pop(0x9200, 0x9204); });
        f.gates.WaitReached("m.before_pop_cas");
        g_clock += 200;
        const auto c2 = f.Collect(0, 0x9300);
        CHECK(c2.registered);
        f.gates.Release("m.before_pop_cas");
        pt.join();
        CHECK(f.C(f.c().observation_lost) == 1);
        CHECK(f.C(f.c().pop_over_notice) == 0);
        CHECK((f.proto.ledger.Marks(*c2.handle.slot) & kBitPopped) == 0); // never marks a new generation
    }
    SECTION("while the application marks t_apply") {
        Fx f(1);
        f.proto.ledger.expiry_us = 100;
        f.Collect(0, 0x9400);
        const Proto::Batch b = f.GatherAll();
        f.gates.Arm("m.before_apply_cas");
        std::thread at([&] { f.ApplyBatchRanges(b); });
        f.gates.WaitReached("m.before_apply_cas");
        g_clock += 200;
        const auto c2 = f.Collect(0, 0x9500);
        CHECK(c2.registered);
        f.gates.Release("m.before_apply_cas");
        at.join();
        CHECK(f.C(f.c().observation_lost) == 1);
        CHECK(f.C(f.c().notice_applied) == 0);
        CHECK((f.proto.ledger.Marks(*c2.handle.slot) & kBitApplied) == 0); // the new record is pending
        f.Pop(0x9500, 0x9504);
        CHECK(f.C(f.c().pop_over_notice) == 1);
    }
    SECTION("a payload of another generation is never taken for the current one") {
        Fx f;
        const auto c = f.Collect(0, 0x9600);
        f.Pop(0x9600, 0x9604);
        f.proto.ledger.TestWritePopWords(*c.handle.slot, Tag(c.handle.gen + 7), 0x9600, 4, 1);
        f.Invalidate();
        CHECK(f.C(f.c().window_unverified) == 1);
        CHECK(f.C(f.c().window_sample_equal) + f.C(f.c().window_sample_different) == 0);
    }
    SECTION("a pop payload written after the slot was reused is lost, then overwritten by a fresh pop") {
        Fx f(1);
        f.proto.ledger.expiry_us = 100;
        f.read_guest = [](u64, u8* out, size_t len) { std::memcpy(out, kBytes, len); };
        f.Collect(0, 0x9700);
        f.gates.Arm("m.before_pop_payload");
        std::thread pt([&] { f.Pop(0x9700, 0x9704); });
        f.gates.WaitReached("m.before_pop_payload");
        g_clock += 200;
        f.Collect(0, 0x9800);
        f.gates.Release("m.before_pop_payload"); // writes the old generation's words, then loses the CAS
        pt.join();
        CHECK(f.C(f.c().observation_lost) == 1);
        f.Pop(0x9800, 0x9804); // a fresh pop overwrites them
        CHECK(f.C(f.c().pop_over_notice) == 1);
        f.Invalidate();
        CHECK(f.C(f.c().window_sample_equal) == 1);
        CHECK(f.C(f.c().window_unverified) == 0);
    }
}

TEST_CASE("list diag: the label of a generation is never zero and the slot keeps working past 65535",
          "[list-diag]") {
    bool never_zero = true;
    for (u64 g = 0; g < 200000; ++g) {
        never_zero = never_zero && Tag(g) != 0 && Tag(g) <= 0xffff;
    }
    CHECK(never_zero);
    // The generation of a claim is full width: only the operations that compare a label take it from there.
    // 65534: label 0xffff; 65535: the label wraps; 65536: the 16 low bits of the generation are zero;
    // 65537: past it.
    for (const u64 gen : {u64{65534}, u64{65535}, u64{65536}, u64{65537}, u64{131071}, u64{131072}}) {
        INFO("claim generation " << gen);
        { // normal publication, pop, application, release
            Fx f(1);
            f.read_guest = [](u64, u8* out, size_t len) { std::memcpy(out, kBytes, len); };
            f.proto.ledger.TestSetGeneration(0, 0, gen);
            const auto c = f.Collect(0, 0x1000);
            CHECK(c.registered);
            CHECK(c.handle.gen == gen);
            f.Pop(0x1000, 0x1004);
            CHECK(f.C(f.c().pop_over_notice) == 1);
            f.Invalidate();
            CHECK(f.C(f.c().notice_applied) == 1);
            CHECK(f.C(f.c().window_sample_equal) == 1); // the pop payload of this generation is valid
            CHECK(f.C(f.c().window_unverified) == 0);
            CHECK(f.PublishedSlots() == 0);
            CHECK(f.Conserved());
        }
        { // published late: Late and then Free
            Fx f(1);
            f.proto.ledger.TestSetGeneration(0, 0, gen);
            f.gates.Arm("w.after_s2");
            Proto::Claim c;
            std::thread wt([&] { c = f.Collect(0, 0x1100); });
            f.gates.WaitReached("w.after_s2");
            f.Invalidate();
            f.gates.Release("w.after_s2");
            wt.join();
            CHECK(f.C(f.c().notice_late) == 1);
            CHECK(f.PublishedSlots() == 0); // the release must succeed past generation 65535
            CHECK(f.Conserved());
        }
        { // expiry: the slot is reused while the old record is still published
            Fx f(1);
            f.proto.ledger.expiry_us = 100;
            f.proto.ledger.TestSetGeneration(0, 0, gen);
            f.Collect(0, 0x1200);
            g_clock += 200;
            const auto c2 = f.Collect(0, 0x1300);
            CHECK(c2.registered);
            CHECK(c2.handle.gen == gen + 1);
            CHECK(f.C(f.c().notice_expired) == 1);
            f.Invalidate(); // lot 2 holds both; only the live record is retired
            CHECK(f.PublishedSlots() == 0);
            CHECK(f.Conserved());
        }
        { // a reader of the previous generation cannot mark the new one
            Fx f(1);
            f.proto.ledger.expiry_us = 100;
            f.proto.ledger.TestSetGeneration(0, 0, gen);
            f.Collect(0, 0x1400);
            f.gates.Arm("m.before_pop_cas");
            std::thread pt([&] { f.Pop(0x1400, 0x1404); });
            f.gates.WaitReached("m.before_pop_cas");
            g_clock += 200;
            const auto c2 = f.Collect(0, 0x1500);
            f.gates.Release("m.before_pop_cas");
            pt.join();
            CHECK(f.C(f.c().observation_lost) == 1);
            CHECK((f.proto.ledger.Marks(*c2.handle.slot) & kBitPopped) == 0);
            CHECK(f.Conserved());
        }
    }
}

// =============================================================================================
// Precondition P0, and what the report says when it is broken.

TEST_CASE("list diag: two Gathers at once are counted, the lots up to there are suspect, the report says so once",
          "[list-diag]") {
    Fx f;
    g_skip_overlap = true; // the race on front_buffer itself is not executed
    const auto c = f.Collect(0, 0xC000);
    CHECK(c.lot == 1);
    CHECK(!f.proto.P0Violated());
    f.gates.Arm("g.before_iter");
    Proto::Batch b;
    std::thread at([&] { b = f.GatherAll(); });
    f.gates.WaitReached("g.before_iter");
    u64 lot = 99;
    std::function<void(PAddr, size_t)> cb = [](PAddr, size_t) {};
    f.managers[0].GatherT<true, TestHooks>(cb, &f.proto.lots[0], &lot); // a second Gather while the first iterates
    CHECK(lot == 0);
    CHECK(f.proto.lots[0].gather_overlapped.load() == 1);
    f.gates.Release("g.before_iter");
    at.join();
    f.Pop(0xC000, 0xC004);
    CHECK(f.C(f.c().pop_over_ambiguous) == 1); // lot 1 <= floor: no window claimed
    CHECK(f.C(f.c().pop_over_notice) == 0);

    // The report: P0 is flagged, once in the log, and the summary repeats it.
    f.proto.CheckViolation();
    CHECK(f.proto.P0Violated());
    f.proto.CheckViolation(); // a second time: nothing new
    const auto lines = f.Lines();
    unsigned notices = 0;
    for (const auto& l : lines) {
        if (l.find("precondicao P0 violada") != std::string::npos) ++notices;
    }
    CHECK(notices == 1);
    CHECK(Contains(lines, "os resultados do diagnostico a partir daqui nao sao confiaveis"));
    const auto summary = f.proto.SummaryLines();
    CHECK(Contains(summary, "precondicao P0 violada"));
    CHECK(Contains(summary, "counters at the first violation"));
    const auto report = f.proto.ReportLines(4.0);
    REQUIRE(!report.empty());
    CHECK(Contains(report, "P0 VIOLATED"));
    // The counters go on after the violation.
    const u64 before = f.C(f.c().notice_registered);
    f.Collect(1, 0xC800);
    CHECK(f.C(f.c().notice_registered) == before + 1);
}

TEST_CASE("list diag: an application out of order is a violation of P0 too", "[list-diag]") {
    Fx f;
    f.Collect(0, 0xC100);
    const Proto::Batch b1 = f.GatherAll();
    const Proto::Batch b2 = f.GatherAll();
    f.ApplyBatchRanges(b2); // lot 2 first
    f.ApplyBatchRanges(b1); // lot 1 after lot 2
    CHECK(f.C(f.c().apply_out_of_order) == 3); // one per manager: all three were applied out of order
    CHECK(f.proto.P0Violated());
}

TEST_CASE("list diag: without a violation the report does not mention P0", "[list-diag]") {
    Fx f;
    f.Collect(0, 0xC200);
    f.Invalidate();
    CHECK(!f.proto.P0Violated());
    const auto report = f.proto.ReportLines(4.0);
    CHECK(!report.empty());
    CHECK(!Contains(report, "P0"));
    CHECK(!Contains(f.proto.SummaryLines(), "P0"));
}

// =============================================================================================
// The ranges that OnCachedWrites receives after the join: the set of (core, lot).

TEST_CASE("list diag: a joined range carries the set of (core, lot) of its notices", "[list-diag]") {
    Fx f;
    f.Collect(0, 0x5000);
    f.Collect(1, 0x5040);
    const Proto::Batch b = f.GatherAll();
    REQUIRE(b.merged.size() == 1);
    CHECK(b.merged[0].mask == 3u);
    const auto c = f.Collect(2, 0x5020); // core 2, inside the joined range
    CHECK(c.lot == 2);
    f.ApplyBatchRanges(b);
    CHECK(f.C(f.c().notice_applied) == 2);          // cores 0 and 1, through one range
    CHECK(f.C(f.c().apply_overlap_other_lot) == 1); // core 2's notice is of lot 2
}

// =============================================================================================
// Part pop: the sample, the labels and the log.

TEST_CASE("list diag: sample_equal is consistent with an overwrite and with a memcpy not yet done",
          "[list-diag]") {
    Fx f;
    std::map<u64, std::array<u8, 4>> guest;
    f.read_guest = [&](u64 addr, u8* out, size_t len) { std::memcpy(out, guest[addr].data(), len); };
    f.Collect(0, 0xE000);
    f.Collect(0, 0xE100);
    guest[0xE000] = {1, 2, 3, 4}; // the memcpy has not landed: the guest still holds the pop's bytes
    guest[0xE100] = {9, 9, 9, 9}; // newer bytes are there
    f.Pop(0xE000, 0xE004);
    f.Pop(0xE100, 0xE104);
    f.Invalidate();
    CHECK(f.C(f.c().window_sample_equal) == 1);
    CHECK(f.C(f.c().window_sample_different) == 1);
    CHECK(f.C(f.c().window_sample_bytes) == 8);
    const auto lines = f.Lines();
    CHECK(Contains(lines, "sample=sample_equal (sample=4B)"));
    CHECK(Contains(lines, "sample=sample_different (sample=4B)"));
    CHECK(Contains(lines, "[ListDiag][pop] hit: notice=[0xe000,+4)"));
    // The names say what was seen, never "lost" or "benign".
    CHECK(!Contains(lines, "benign"));
    CHECK(!Contains(lines, "perda"));
}

TEST_CASE("list diag: a pop before the notice, or after its application, is no event", "[list-diag]") {
    Fx f;
    f.Pop(0xF000, 0xF004); // before the notice exists
    CHECK(f.C(f.c().pop_over_notice) == 0);
    f.Collect(0, 0xF000);
    f.Invalidate();
    f.Pop(0xF000, 0xF004); // after the application
    CHECK(f.C(f.c().pop_over_notice) == 0);
    CHECK(f.C(f.c().pop_ranges) == 2);
}

TEST_CASE("list diag: the log is limited: the first 32 of each kind, then one in 64", "[list-diag]") {
    Fx f(256);
    for (u64 i = 0; i < 200; ++i) {
        f.Collect(0, 0x100000 + i * 64);
    }
    for (u64 i = 0; i < 200; ++i) {
        const u64 a = 0x100000 + i * 64;
        f.Pop(a, a + 4);
    }
    CHECK(f.C(f.c().pop_over_notice_records) == 200);
    const auto lines = f.Lines();
    // n = 0..199: the first 32, then n = 32, 96, 160.
    CHECK(lines.size() == 32 + 3);
    CHECK(f.C(f.c().log_dropped) == 0);
    // Counters are not limited.
    CHECK(f.C(f.c().pop_over_notice) == 200);
}

TEST_CASE("list diag: a notice that a pop reached and that expired without being applied is logged",
          "[list-diag]") {
    Fx f(1);
    f.proto.ledger.expiry_us = 100;
    f.Collect(0, 0xA000);
    f.Pop(0xA000, 0xA004);
    g_clock += 200;
    f.Collect(0, 0xA100); // reclaims the slot
    CHECK(f.C(f.c().notice_expired) == 1);
    CHECK(Contains(f.Lines(), "apply=never"));
}

// =============================================================================================
// Part blk: the 64-byte block.

TEST_CASE("list diag: the real Gather rounds a notice to a 64-byte block", "[list-diag]") {
    Manager m;
    m.Collect(0x10008, 4);
    std::vector<std::pair<u64, u64>> out;
    std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { out.emplace_back(a, s); };
    m.Gather(cb);
    REQUIRE(out.size() == 1);
    CHECK(out[0].first == 0x10000);
    CHECK(out[0].second == 64);
}

TEST_CASE("list diag: problem B, the application covers GPU bytes the CPU did not write", "[list-diag]") {
    SECTION("64 bytes of the GPU in the block, 4 written by the CPU: 60 at risk (CPU read tracked)") {
        Fx f;
        f.track_cpu_reads = true;
        f.gpu_modified.Add(0x20000, 64);
        f.cpu_read.Add(0x20000, 16); // the CPU read [0x20000, 0x20010); the exact write is [0x20008, 0x2000c)
        f.Collect(0, 0x20008);
        f.Invalidate();
        CHECK(f.C(f.c().blk_applies) == 1);
        CHECK(f.C(f.c().blk_applies_over_gpu) == 1);
        CHECK(f.C(f.c().blk_bytes_range) == 64);
        CHECK(f.C(f.c().blk_bytes_exact) == 4);
        CHECK(f.C(f.c().blk_gpu_bytes_covered) == 64);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 60);
        // cpu_read covers [0x20000, 0x20010) = 16 bytes, 4 of them are the exact write: 12 at risk and read.
        CHECK(f.C(f.c().blk_at_risk_cpu_read) == 12);
        CHECK(f.C(f.c().blk_at_risk_not_seen_cpu_read) == 48);
        CHECK(f.C(f.c().blk_at_risk_unknown) == 0);
        CHECK(f.C(f.c().blk_ambiguous) == 0);
        const auto lines = f.Lines();
        CHECK(Contains(lines, "[ListDiag][blk] range=[0x20000,+64) blocks=1/64"));
        CHECK(Contains(lines, "gpu_covered=64 at_risk=60 (cpu_read=12 not_seen_cpu_read=48 unknown=0)"));
    }
    SECTION("without the CPU read tracking (not Switch 2) the at-risk bytes are unknown") {
        Fx f;
        f.track_cpu_reads = false;
        f.gpu_modified.Add(0x21000, 64);
        f.Collect(0, 0x21000);
        f.Invalidate();
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 60);
        CHECK(f.C(f.c().blk_at_risk_unknown) == 60);
        CHECK(f.C(f.c().blk_at_risk_cpu_read) == 0);
    }
    SECTION("no GPU bytes in the block: nothing at risk") {
        Fx f;
        f.Collect(0, 0x22000);
        f.Invalidate();
        CHECK(f.C(f.c().blk_applies) == 1);
        CHECK(f.C(f.c().blk_applies_over_gpu) == 0);
        CHECK(f.C(f.c().blk_gpu_bytes_covered) == 0);
        CHECK(f.C(f.c().blk_bytes_exact) == 4);
    }
    SECTION("GPU bytes only where the CPU wrote: the exact write is not at risk") {
        Fx f;
        f.gpu_modified.Add(0x23004, 4);
        f.Collect(0, 0x23004);
        f.Invalidate();
        CHECK(f.C(f.c().blk_gpu_bytes_covered) == 4);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
        CHECK(f.C(f.c().blk_ambiguous) == 0);
    }
    SECTION("two notices of the same lot in one block: the union of the exact writes is discounted") {
        Fx f;
        f.gpu_modified.Add(0x24000, 64);
        f.Collect(0, 0x24000);
        f.Collect(0, 0x24020);
        f.Invalidate();
        CHECK(f.C(f.c().blk_bytes_exact) == 8);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 56);
    }
    SECTION("a notice of another lot in the block makes the result ambiguous, not a number") {
        Fx f;
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        const Proto::Batch b1 = f.GatherAll();
        f.Collect(0, 0x25008); // registered after lot 1 was taken: lot 2
        f.ApplyBatchRanges(b1);
        CHECK(f.C(f.c().blk_ambiguous) == 1);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
        CHECK(f.C(f.c().blk_gpu_bytes_covered) == 64); // the upper bound is still reported
        CHECK(Contains(f.Lines(), "at_risk=ambiguous"));
    }
    SECTION("an application that the Gather did not hand over is unlinked and retires nothing") {
        Fx f;
        f.Collect(0, 0x26000);
        std::lock_guard lock{f.cache_mutex};
        f.proto.NoteApply(0x26000, 64, f.gpu_modified, f.cpu_read, false, f.read_guest); // no batch published
        CHECK(f.C(f.c().apply_unlinked) == 1);
        CHECK(f.C(f.c().notice_applied) == 0);
    }
    SECTION("the part blk alone: no pop counters move") {
        Fx f;
        f.proto.pop_on = false;
        f.gpu_modified.Add(0x27000, 64);
        f.Collect(0, 0x27000);
        f.Invalidate();
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 60);
        CHECK(f.C(f.c().pop_ranges) == 0);
        CHECK(f.C(f.c().window_unverified) == 0);
    }
}

TEST_CASE("list diag: with the part blk off, the application only retires notices", "[list-diag]") {
    Fx f;
    f.proto.blk_on = false;
    f.gpu_modified.Add(0x28000, 64);
    f.Collect(0, 0x28000);
    f.Invalidate();
    CHECK(f.C(f.c().notice_applied) == 1);
    CHECK(f.C(f.c().blk_applies) == 0);
}

// =============================================================================================
// Off: nothing changes. On: the production wrappers.

TEST_CASE("list diag: the flag parser", "[list-diag]") {
    CHECK(ParseParts(nullptr) == 0);
    CHECK(ParseParts("") == 0);
    CHECK(ParseParts("0") == 0);
    CHECK(ParseParts("1") == (PartPop | PartBlk | PartList));
    CHECK(ParseParts("pop") == PartPop);
    CHECK(ParseParts("pop,blk") == (PartPop | PartBlk));
    CHECK(ParseParts("blk,list") == (PartBlk | PartList));
    CHECK(ParseParts("popular") == 0); // a prefix is not a part
}

TEST_CASE("list diag: off, nothing changes; on, the production hooks work", "[list-diag]") {
    const u32 saved = g_parts.load();
    g_parts = 0;
    REQUIRE(g_state.load() == nullptr);
    // The same script through the production wrapper and through the plain manager.
    Manager wrapped, plain;
    std::mt19937 rng{77};
    using Out = std::vector<std::pair<u64, u64>>;
    bool same = true;
    for (int step = 0; step < 5000; ++step) {
        if (rng() % 5 == 0) {
            Out a, b;
            std::function<void(PAddr, size_t)> ca = [&](PAddr x, size_t y) { a.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cb = [&](PAddr x, size_t y) { b.emplace_back(x, y); };
            wrapped.Gather(ca);
            plain.Gather(cb);
            same = same && a == b;
        } else {
            const u64 addr = 0x30000 + (rng() % 8) * 4096 + (rng() % 400) * 4;
            CollectNotice(wrapped, 0, addr, 4);
            plain.Collect(addr, 4);
        }
    }
    CHECK(same);
    // Off: the other hooks do nothing either, and no state was ever allocated.
    Common::RangeSet<u64> gpu, cpu;
    gpu.Add(0x30000, 64);
    NoteApplyHook(0x30000, 64, gpu, cpu, false, [](u64, u8*, size_t) {});
    NotePopHook(0x30000, 0x30004, kBytes);
    DrainLog();
    CHECK(ReportLines(1.0).empty());
    CHECK(g_state.load() == nullptr);

    // On (pop and blk): the production protocol, with the steady clock.
    g_parts = PartPop | PartBlk;
    Manager manager;
    CollectNotice(manager, 0, 0x31008, 4);
    REQUIRE(g_state.load() != nullptr);
    Impl& state = State();
    CHECK(Get(state.counters.notice_registered) == 1);
    NotePopHook(0x31008, 0x3100c, kBytes);
    CHECK(Get(state.counters.pop_over_notice) == 1);
    CHECK(!ReportLines(1.0).empty());
    g_parts = saved;
}

// This test creates the state of the diagnostic: it must come after the one that checks that nothing
// is allocated while the diagnostic is off (the tests run in the order they are written).
TEST_CASE("list diag: a line says the diagnostic is on, and the totals line carries a tag", "[list-diag]") {
    const u32 saved = g_parts.load();
    g_parts = PartPop | PartBlk;
    State(); // created (and its log sink installed) before the capture below
    static std::vector<std::string> captured;
    captured.clear();
    const LogSink saved_sink = g_sink.load();
    g_sink = [](const std::string& line) { captured.push_back(line); };
    LogStartup();
    REQUIRE(captured.size() == 1);
    CHECK(captured[0].find("[ListDiag] ligado: partes=pop,blk") == 0);
    CHECK(captured[0].find("list: T3b") == std::string::npos);
    const auto totals = State().SummaryLines("total");
    CHECK(!totals.empty());
    CHECK(totals[0].find("[ListDiag][total]") == 0);
    g_parts = 0;
    captured.clear();
    LogStartup(); // off: silence
    CHECK(captured.empty());
    g_sink = saved_sink;
    g_parts = saved;
}

// =============================================================================================
// Threads: writers, one invalidator at a time (the precondition), a pop thread; the ground truth
// is the Gather itself.

namespace {

struct StressConfig {
    unsigned notices = 800;
    unsigned slots = 64;
    u64 expiry = 1'000'000;
    unsigned seed = 1;
    long long mutant = 0; // claim the wrong lot: the ground truth must notice
};

struct Recorded {
    u64 addr;
    Proto::Claim claim;
};

struct StressResult {
    u64 bad_claims = 0, missing = 0, false_windows = 0, registered = 0, unambiguous = 0, ambiguous = 0;
    u64 overlapped = 0;
    bool conserved = false;
    u64 total = 0;
};

StressResult RunStress(const StressConfig& cfg) {
    auto fx = std::make_unique<Fx>(cfg.slots);
    Fx& f = *fx;
    f.proto.ledger.expiry_us = cfg.expiry;
    g_mutant = cfg.mutant;
    std::unordered_set<u64> applied_blocks; // under cache_mutex
    std::atomic<u64> false_windows{0};
    f.on_range_applied = [&](u64 addr, u64 size) {
        for (u64 a = addr & ~u64{63}; a < addr + size; a += 64) {
            applied_blocks.insert(a);
        }
    };
    g_on_window_flag = [&](u64 addr, u32) { // called by NotePop under cache_mutex
        if (applied_blocks.count(addr & ~u64{63}) != 0) {
            false_windows.fetch_add(1);
        }
    };

    std::vector<std::vector<Recorded>> recorded(kCores);
    std::array<std::unordered_map<u64, u64>, kCores> first_lot; // block -> first lot that delivered it
    std::mutex invalidate_mutex; // the precondition: one Gather at a time (models the Drain)
    std::atomic<unsigned> writers_done{0};
    std::array<std::atomic<u64>, 64> recent{};
    std::atomic<u64> recent_index{0};

    auto invalidate = [&] {
        std::lock_guard lock{invalidate_mutex};
        const Proto::Batch b = f.GatherAll();
        for (const Range& r : b.pre) {
            for (unsigned core = 0; core < kCores; ++core) {
                if ((r.mask >> core) & 1) {
                    for (u64 a = r.addr; a < r.addr + r.size; a += 64) {
                        first_lot[core].emplace(a, b.lots[core]);
                    }
                }
            }
        }
        f.ApplyBatchRanges(b);
    };

    std::vector<std::thread> threads;
    for (unsigned core = 0; core < kCores; ++core) {
        threads.emplace_back([&, core] {
            std::mt19937 rng{cfg.seed * 100 + core};
            for (unsigned i = 0; i < cfg.notices; ++i) {
                const u64 id = core * 1'000'000ull + i;
                const u64 addr = 0x100000 + id * 64 + (i % 16) * 4;
                const auto c = f.Collect(core, addr);
                recorded[core].push_back({addr, c});
                recent[recent_index.fetch_add(1) % recent.size()].store(addr);
                g_clock.fetch_add(10);
                if (rng() % 8 == 0) {
                    std::this_thread::yield();
                }
            }
            writers_done.fetch_add(1);
        });
    }
    for (int k = 0; k < 2; ++k) { // "front" and "back": never at the same time
        threads.emplace_back([&, k] {
            std::mt19937 rng{cfg.seed * 7 + static_cast<unsigned>(k)};
            while (writers_done.load() < kCores) {
                invalidate();
                if (rng() % 3 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    threads.emplace_back([&] { // the fence thread
        std::mt19937 rng{cfg.seed * 13};
        std::array<u8, 64> bytes{};
        while (writers_done.load() < kCores) {
            const u64 addr = recent[rng() % recent.size()].load();
            if (addr != 0) {
                const u64 block = addr & ~u64{63};
                f.Pop(block, block + 64, bytes.data());
            }
            if (rng() % 4 == 0) {
                std::this_thread::yield();
            }
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    for (int k = 0; k < 3; ++k) {
        invalidate();
    }

    StressResult res;
    for (unsigned core = 0; core < kCores; ++core) {
        for (const Recorded& r : recorded[core]) {
            if (!r.claim.registered) continue;
            ++res.registered;
            if (r.claim.ambiguous) {
                ++res.ambiguous;
                continue;
            }
            ++res.unambiguous;
            const auto it = first_lot[core].find(r.addr & ~u64{63});
            if (it == first_lot[core].end()) {
                ++res.missing;
            } else if (it->second != r.claim.lot) {
                ++res.bad_claims;
            }
        }
    }
    res.false_windows = false_windows.load();
    res.total = static_cast<u64>(cfg.notices) * kCores;
    res.conserved = f.Conserved() && res.registered + Get(f.c().notice_overflow) == res.total &&
                    res.registered == Get(f.c().notice_registered);
    for (unsigned core = 0; core < kCores; ++core) {
        res.overlapped += f.proto.lots[core].gather_overlapped.load();
    }
    res.overlapped += Get(f.c().apply_out_of_order);
    return res;
}

unsigned StressNotices() {
    const char* value = std::getenv("LIST_DIAG_STRESS_NOTICES");
    return value != nullptr ? static_cast<unsigned>(std::atoi(value)) : 800;
}

} // namespace

TEST_CASE("list diag: stress with threads, ground truth from the Gather", "[list-diag][stress]") {
    const unsigned notices = StressNotices();
    for (unsigned round = 1; round <= 3; ++round) {
        INFO("round " << round);
        const StressResult roomy = RunStress({notices, 4096, 1'000'000'000ull, round, 0});
        CHECK(roomy.bad_claims == 0); // every unambiguous lot is the first that delivered the block
        CHECK(roomy.missing == 0);
        CHECK(roomy.false_windows == 0); // a pop never flags a record whose block was already applied
        CHECK(roomy.conserved);          // every notice has exactly one end
        CHECK(roomy.overlapped == 0);    // the test respects P0
        CHECK(roomy.unambiguous > 0);

        const StressResult tight = RunStress({notices, 8, 400, round + 100, 0});
        CHECK(tight.bad_claims == 0);
        CHECK(tight.missing == 0);
        CHECK(tight.false_windows == 0);
        CHECK(tight.conserved);
        CHECK(tight.overlapped == 0);
    }
    // The ground truth is not vacuous: a wrong lot is seen.
    const StressResult mutant = RunStress({notices, 4096, 1'000'000'000ull, 99, 1});
    CHECK(mutant.bad_claims + mutant.missing > 0);
}

// =============================================================================================
// D1 (docs/gpu-contract-inventory.md, 7.4.1): CollectT splits a write that crosses the end of a 2 KiB
// transformation. The expected result is the union of the written bytes rounded out to 64-byte
// blocks; the output of the old behaviour for a crossing write is undefined and is NOT tested.

namespace {

using Blocks = std::set<u64>; ///< 64-byte block numbers

Blocks ExpectedBlocks(u64 addr, u64 size) {
    Blocks blocks;
    for (u64 b = addr / 64; b <= (addr + size - 1) / 64; ++b) {
        blocks.insert(b);
    }
    return blocks;
}

template <bool Diag, class M>
Blocks Delivered(M& manager, std::vector<std::pair<u64, u64>>* ranges = nullptr) {
    Blocks blocks;
    std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) {
        REQUIRE(a % 64 == 0);
        REQUIRE(s % 64 == 0);
        if (ranges != nullptr) {
            ranges->emplace_back(a, s);
        }
        for (u64 b = a / 64; b < (a + s) / 64; ++b) {
            blocks.insert(b); // the same block may come twice (entries of different Collects): a union
        }
    };
    if constexpr (Diag) {
        Core::GatherLotState lots;
        u64 lot = 0;
        manager.template GatherT<true, TestHooks>(cb, &lots, &lot);
    } else {
        manager.Gather(cb);
    }
    return blocks;
}

template <bool Diag>
void CollectAndCheck(u64 addr, u64 size) {
    INFO("addr=0x" << std::hex << addr << " size=" << std::dec << size << " diag=" << Diag);
    Manager manager;
    manager.template CollectT<Diag, TestHooks>(addr, size);
    CHECK(Delivered<Diag>(manager) == ExpectedBlocks(addr, size));
}

template <bool Diag>
void RunD1Cases() {
    // Exact 2 KiB, both halves; a whole 4 KiB page.
    CollectAndCheck<Diag>(0x10000, 2048);
    CollectAndCheck<Diag>(0x10800, 2048);
    CollectAndCheck<Diag>(0x10000, 4096);
    // Unaligned start, crossing: 2 pieces; and starting in the second half.
    CollectAndCheck<Diag>(0x10033, 3000);
    CollectAndCheck<Diag>(0x107ff, 2);
    CollectAndCheck<Diag>(0x10801, 3000);
    // Short crossing.
    CollectAndCheck<Diag>(0x107c0, 128);
    // The cases of the table of 7.4.
    CollectAndCheck<Diag>(0x10400, 2048);
    CollectAndCheck<Diag>(0x10000, 3000);
}

} // namespace

TEST_CASE("list diag D1: crossing and exact cases deliver the whole range, rounded to 64 bytes", "[list-diag][d1]") {
    RunD1Cases<false>();
    RunD1Cases<true>();
}

TEST_CASE("list diag D1: the short crossing is two ranges of 64 bytes", "[list-diag][d1]") {
    Manager manager;
    manager.CollectT<false>(0x107c0, 128);
    std::vector<std::pair<u64, u64>> ranges;
    Delivered<false>(manager, &ranges);
    std::sort(ranges.begin(), ranges.end());
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0] == std::pair<u64, u64>{0x107c0, 64});
    CHECK(ranges[1] == std::pair<u64, u64>{0x10800, 64});
}

TEST_CASE("list diag D1: random writes inside a 4 KiB page are covered, not more than the 64-byte rounding",
          "[list-diag][d1]") {
    std::mt19937 rng{777};
    for (int i = 0; i < 20000; ++i) {
        const u64 page = 0x40000 + (rng() % 4) * 4096;
        const u64 off = rng() % 4096;
        const u64 size = 1 + rng() % (4096 - off);
        if (i % 2 == 0) {
            CollectAndCheck<false>(page + off, size);
        } else {
            CollectAndCheck<true>(page + off, size);
        }
    }
}

TEST_CASE("list diag D1: several writes in one manager give the union", "[list-diag][d1]") {
    std::mt19937 rng{4242};
    for (int round = 0; round < 200; ++round) {
        Manager manager;
        Blocks expected;
        for (int k = 0; k < 6; ++k) {
            const u64 page = 0x50000 + (rng() % 3) * 4096;
            const u64 off = rng() % 4096;
            const u64 size = 1 + rng() % (4096 - off);
            manager.CollectT<true, TestHooks>(page + off, size);
            const Blocks b = ExpectedBlocks(page + off, size);
            expected.insert(b.begin(), b.end());
        }
        CHECK(Delivered<true>(manager) == expected);
    }
}

TEST_CASE("list diag D1: size zero changes nothing", "[list-diag][d1]") {
    for (const u64 addr : {u64{0x10000}, u64{0x107c0}, u64{0x107ff}, u64{0x10800}, u64{0x10fff}}) {
        Manager manager;
        manager.CollectT<false, TestHooks>(addr, 0);
        manager.CollectT<true, TestHooks>(addr, 0);
        CHECK(Delivered<false>(manager).empty());
    }
}

TEST_CASE("list diag D1: writes that do not cross 2 KiB equal the manager of before",
          "[list-diag][d1]") {
    Legacy::GPUDirtyMemoryManager legacy;
    Manager plain;
    Manager diagnostic;
    std::mt19937 rng{99};
    using Out = std::vector<std::pair<u64, u64>>;
    bool all_equal = true;
    for (int step = 0; step < 20000; ++step) {
        if (rng() % 7 == 0) {
            Out a, b, c;
            std::function<void(PAddr, size_t)> ca = [&](PAddr x, size_t y) { a.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cb = [&](PAddr x, size_t y) { b.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cc = [&](PAddr x, size_t y) { c.emplace_back(x, y); };
            legacy.Gather(ca);
            plain.Gather(cb);
            Core::GatherLotState lots;
            u64 lot = 0;
            diagnostic.GatherT<true, TestHooks>(cc, &lots, &lot);
            all_equal = all_equal && a == b && a == c;
        } else {
            const u64 addr0 = 0x10000 + (rng() % 6) * 4096 + (rng() % 2) * 2048;
            const u64 off_in = (rng() % 480) * 4;
            const u64 size = 1 + rng() % std::min<u64>(96, 2048 - off_in);
            legacy.Collect(addr0 + off_in, size);
            plain.Collect(addr0 + off_in, size);
            diagnostic.CollectT<true, TestHooks>(addr0 + off_in, size);
        }
    }
    CHECK(all_equal);
}

TEST_CASE("list diag D1: CrossesTransform, by subtraction",
          "[list-diag][d1]") {
    STATIC_REQUIRE(!Core::GPUDirtyMemoryManager::CrossesTransform(0x10000, 2048));
    STATIC_REQUIRE(!Core::GPUDirtyMemoryManager::CrossesTransform(0x10800, 2048));
    STATIC_REQUIRE(Core::GPUDirtyMemoryManager::CrossesTransform(0x107c0, 128));
    STATIC_REQUIRE(Core::GPUDirtyMemoryManager::CrossesTransform(0x10000, 2049));
    // A huge size cannot wrap the comparison.
    STATIC_REQUIRE(Core::GPUDirtyMemoryManager::CrossesTransform(0x107ff, ~size_t{0}));
}

TEST_CASE("list diag D1: a Gather between the two pieces of a notice makes it ambiguous", "[list-diag][d1]") {
    SECTION("Gather between the pieces") {
        Fx f;
        f.gates.Arm("c.between_pieces");
        Proto::Claim c;
        std::thread wt([&] { c = f.Collect(0, 0x107c0, 128); });
        f.gates.WaitReached("c.between_pieces");
        f.gpu_modified.Add(0x107c0, 128);
        const Proto::Batch first = f.GatherAll(); // takes the first piece, lot 1
        REQUIRE(first.pre.size() == 1);
        CHECK(first.pre[0].addr == 0x107c0);
        CHECK(first.pre[0].size == 64);
        f.ApplyBatchRanges(first);
        f.gates.Release("c.between_pieces");
        wt.join();
        CHECK(c.ambiguous);
        CHECK(c.lot == 0); // never one lot for a notice split between lots
        const Proto::Batch second = f.GatherAll(); // takes the second piece, lot 2
        REQUIRE(second.pre.size() == 1);
        CHECK(second.pre[0].addr == 0x10800);
        f.ApplyBatchRanges(second);
        // Neither lot retires or affirms it.
        CHECK(f.C(f.c().notice_ambiguous) == 1);
        CHECK(f.C(f.c().notice_applied) == 0);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
    }
    SECTION("no Gather between the pieces: both pieces are of lot d + 1") {
        Fx f;
        const auto c = f.Collect(0, 0x107c0, 128);
        CHECK(!c.ambiguous);
        CHECK(c.lot == 1);
        const Proto::Batch b = f.GatherAll();
        CHECK(b.pre.size() == 2);
        f.ApplyBatchRanges(b);
        CHECK(f.C(f.c().notice_applied) == 1);
    }
}
