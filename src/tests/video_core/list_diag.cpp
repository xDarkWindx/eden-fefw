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
#include <cmath>
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
#include "list_diag_t3a_frozen.h"

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
inline std::atomic<bool> g_no_coverage{false};  // mutant NaiveNoCoverage
inline std::atomic<bool> g_no_open_flag{false}; // mutant NaiveNoOpenFlag
inline std::atomic<u64> g_clock_reads{0};
/// LIST_DIAG_FORCE_MUTANT=coverage|open turns a mutant on in EVERY test: the harness is run that way to
/// count the tests that notice it (they must fail).
inline bool ForcedMutant(const char* which) {
    const char* value = std::getenv("LIST_DIAG_FORCE_MUTANT");
    return value != nullptr && std::strcmp(value, which) == 0;
}
inline std::function<void(u64, u64, u32, const u64*, const std::vector<std::pair<u64, u64>>&)> g_on_affirmed;
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
    static void OnAffirmed(u64 addr, u64 size, u32 mask, const u64* lots,
                           const std::vector<std::pair<u64, u64>>& exact) {
        if (g_on_affirmed) {
            g_on_affirmed(addr, size, mask, lots, exact);
        }
    }
    static bool NaiveNoCoverage() {
        return g_no_coverage.load();
    }
    static bool NaiveNoOpenFlag() {
        return g_no_open_flag.load();
    }
    static void OnWindowFlag(u64 addr, u32 size) {
        if (g_on_window_flag) {
            g_on_window_flag(addr, size);
        }
    }
    static u64 NowUs() {
        g_clock_reads.fetch_add(1, std::memory_order_relaxed);
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
        g_no_coverage = ForcedMutant("coverage");
        g_no_open_flag = ForcedMutant("open");
        g_clock_reads = 0;
        g_on_affirmed = nullptr;
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
        g_no_coverage = false;
        g_no_open_flag = false;
        g_on_affirmed = nullptr;
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
    Proto::NoticeSums W() const {
        return proto.Sums();
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
        const auto w = W();
        return w.registered == Get(c().notice_applied) + w.late + w.expired + PublishedSlots();
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
        CHECK(f.W().late == 1);
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
        CHECK(f.W().late == 0); // already retired: exactly one end state
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
        CHECK(f.W().late == 1);
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
        CHECK(f.W().late == 0);
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
        CHECK(f.W().expired == 1);
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
            CHECK(f.W().late == 1);
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
            CHECK(f.W().expired == 1);
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
    const u64 before = f.W().registered;
    f.Collect(1, 0xC800);
    CHECK(f.W().registered == before + 1);
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
    CHECK(f.W().expired == 1);
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
    Impl& state = State();
    state.sample_n = 1; // the test uses one page; the production default is 1/8 of the pages
    CollectNotice(manager, 0, 0x31008, 4);
    REQUIRE(g_state.load() != nullptr);
    CHECK(state.Sums().registered == 1);
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
    u64 sample_n = 1;
    bool no_coverage = false;  // mutant NaiveNoCoverage
    bool no_open_flag = false; // mutant NaiveNoOpenFlag
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
    u64 affirmed = 0;       ///< ranges the protocol claimed complete
    u64 false_complete = 0; ///< ... in which a delivered notice of the sample is NOT in `exact`
    u64 sampled_seen = 0, sampled_ok = 0;
};

StressResult RunStress(const StressConfig& cfg) {
    auto fx = std::make_unique<Fx>(cfg.slots);
    Fx& f = *fx;
    f.proto.ledger.expiry_us = cfg.expiry;
    f.proto.sample_n = cfg.sample_n;
    g_mutant = cfg.mutant;
    g_no_coverage = cfg.no_coverage || ForcedMutant("coverage");
    g_no_open_flag = cfg.no_open_flag || ForcedMutant("open");
    struct Affirmed {
        u64 addr, size;
        u32 mask;
        std::array<u64, kCores> lots;
        std::vector<std::pair<u64, u64>> exact;
    };
    std::vector<Affirmed> affirmed; // filled under cache_mutex (NoteApply)
    g_on_affirmed = [&](u64 addr, u64 size, u32 mask, const u64* lots, const std::vector<std::pair<u64, u64>>& exact) {
        Affirmed a{addr, size, mask, {}, exact};
        for (unsigned c = 0; c < kCores; ++c) {
            a.lots[c] = lots != nullptr ? lots[c] : 0;
        }
        affirmed.push_back(std::move(a));
    };
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

    f.gpu_modified.Add(0x100000, 0x10000000); // GPU bytes everywhere: every range is accounted
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
    res.conserved = f.Conserved() && res.registered + f.W().overflow == f.W().sampled &&
                    f.W().seen == res.total && res.registered == f.W().registered;
    // No false "complete": every notice of the sample that the Gather delivered for an affirmed range
    // has its bytes inside the exact writes.
    res.affirmed = affirmed.size();
    for (const Affirmed& a : affirmed) {
        for (unsigned core = 0; core < kCores; ++core) {
            if (((a.mask >> core) & 1) == 0) {
                continue;
            }
            for (const Recorded& r : recorded[core]) {
                if (!f.proto.Sampled(r.addr) || r.addr < a.addr || r.addr + 4 > a.addr + a.size) {
                    continue;
                }
                const auto it = first_lot[core].find(r.addr & ~u64{63});
                if (it == first_lot[core].end() || it->second != a.lots[core]) {
                    continue;
                }
                bool inside = false;
                for (const auto& [es, ee] : a.exact) {
                    inside = inside || (es <= r.addr && r.addr + 4 <= ee);
                }
                res.false_complete += inside ? 0 : 1;
            }
        }
    }
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
        CHECK(f.W().ambiguous == 1);
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

// =============================================================================================
// T3a.1 (docs/list-diag-design.md, 3.7): the sample, declared coverage, the pop by page, the hot path.

namespace {

/// First page number >= `from` whose Sampled() verdict is `want`.
u64 PickPage(const Proto& proto, bool want, u64 from = 0x40) {
    for (u64 page = from;; ++page) {
        if (proto.Sampled(page << 12) == want) {
            return page;
        }
    }
}

/// Applies one batch by hand: BeginApply, the ranges, EndApply (the gate tests interleave around it).
void ApplyAll(Fx& f, const Proto::Batch& batch) {
    f.ApplyBatchRanges(batch);
}

} // namespace

TEST_CASE("T3a.1 [1]: sampled and excluded pages mixed in the three paths of Collect", "[list-diag][t3a1]") {
    Fx f(4096);
    f.proto.sample_n = 4;
    Manager reference; // the Collect<false> of before, same sequence
    std::mt19937 rng{31};
    u64 gathers = 0, sampled_count = 0, steps = 0;
    for (int i = 0; i < 3000; ++i) {
        if (rng() % 9 == 0) {
            std::vector<std::pair<u64, u64>> expected;
            std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { expected.emplace_back(a, s); };
            reference.Gather(cb);
            const Proto::Batch b = f.GatherAll();
            std::vector<std::pair<u64, u64>> got;
            for (const Range& r : b.pre) {
                got.emplace_back(r.addr, r.size);
            }
            REQUIRE(got == expected); // what the manager delivers is unchanged by the sample
            ++gathers;
        } else {
            const u64 addr = 0x40000 + (rng() % 6) * 4096 + (rng() % 64) * 64 + (rng() % 16) * 4;
            const auto c = f.Collect(0, addr);
            reference.Collect(addr, 4);
            ++steps;
            CHECK(c.sampled == f.proto.Sampled(addr));
            if (c.sampled) {
                ++sampled_count;
                REQUIRE(c.registered);
                REQUIRE(!c.ambiguous);
                CHECK(c.lot == gathers + 1); // the identity of each notice, from the script
            } else {
                CHECK(!c.registered);
            }
        }
    }
    CHECK(f.W().seen == steps);
    CHECK(f.W().sampled == sampled_count);
    CHECK(f.proto.ledger.LiveBits() == sampled_count); // only the sampled notices have a record
    CHECK(sampled_count > 0);
    CHECK(sampled_count < steps);
}

TEST_CASE("T3a.1 [2]: a notice outside the sample never touches the ledger or the lot counters", "[list-diag][t3a1]") {
    Fx f(64);
    f.proto.sample_n = 8;
    const u64 page = PickPage(f.proto, false);
    constexpr unsigned kNotices = 200;
    for (unsigned i = 0; i < kNotices; ++i) {
        const auto c = f.Collect(i % kCores, (page << 12) + (i % 64) * 64);
        CHECK(!c.sampled);
        CHECK(!c.registered);
    }
    for (unsigned core = 0; core < kCores; ++core) {
        for (unsigned i = 0; i < f.proto.ledger.SlotsPerCore(); ++i) {
            const Slot& s = f.proto.ledger.SlotAt(core, i);
            CHECK(s.state.load() == 0);
            CHECK(s.marks.load() == 0);
            CHECK(s.addr.load() == 0);
        }
        const auto& st = f.proto.lots[core];
        CHECK(st.lot_started.load() == 0);
        CHECK(st.lot_done.load() == 0);
        CHECK(st.apply_started_lot.load() == 0);
        CHECK(st.gather_active.load() == 0);
        const auto& sh = f.proto.writer_shared[core];
        CHECK(sh.open.load() == 0);
        CHECK(sh.incomplete_floor.load() == 0);
    }
    CHECK(f.proto.ledger.LiveBits() == 0);
    for (const FieldRef& field : Fields(f.c())) {
        INFO(field.name);
        CHECK(Get(*field.counter) == 0); // no shared counter moved
    }
    const auto w = f.W();
    CHECK(w.seen == kNotices);
    CHECK(w.sampled + w.registered + w.overflow + w.late + w.expired + w.ambiguous + w.floor_raises == 0);
    CHECK(g_clock_reads.load() == 0);
}

TEST_CASE("T3a.1 [3]: partial and total overflow in the same block and lot", "[list-diag][t3a1]") {
    SECTION("partial: a record found, another lost") {
        Fx f(2);
        f.gpu_modified.Add(0x20000, 64);
        const auto a = f.Collect(0, 0x20000);
        const auto b = f.Collect(0, 0x20010);
        const auto c = f.Collect(0, 0x20020); // the ring has 2 slots: lost
        CHECK(a.registered);
        CHECK(b.registered);
        CHECK(!c.registered);
        CHECK(f.W().overflow == 1);
        ApplyAll(f, f.GatherAll());
        CHECK(f.C(f.c().blk_incomplete) == 1);
        CHECK(f.C(f.c().blk_incomplete_floor) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);          // not claimed
        CHECK(f.C(f.c().blk_incomplete_gpu_bytes) == 64 - 8);  // what T3a would have claimed
    }
    SECTION("the mutant NaiveNoCoverage claims it (the test can see the failure)") {
        Fx f(2);
        g_no_coverage = true;
        f.gpu_modified.Add(0x20000, 64);
        f.Collect(0, 0x20000);
        f.Collect(0, 0x20010);
        f.Collect(0, 0x20020);
        ApplyAll(f, f.GatherAll());
        CHECK(f.C(f.c().blk_affirmed) == 1);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 64 - 8);
    }
    SECTION("total: no record at all in the block is ambiguous, as 5 says") {
        Fx f(1);
        f.gpu_modified.Add(0x21000, 64);
        f.gpu_modified.Add(0x22000, 64);
        CHECK(f.Collect(0, 0x21000).registered);
        CHECK(!f.Collect(0, 0x22000).registered); // same lot, the only slot is taken
        ApplyAll(f, f.GatherAll());
        CHECK(f.C(f.c().blk_ambiguous) == 1); // 0x22000: no record in the block
        // 0x21000 has its record, but the overflow of the same (core, lot) declares the lot incomplete:
        // the granularity is (core, lot), a false "incomplete" that is accepted and counted.
        CHECK(f.C(f.c().blk_incomplete) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
        CHECK(f.C(f.c().blk_ambiguous_no_record) >= 1);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
    }
}

TEST_CASE("T3a.1 [4]: overflow with an ambiguous lot raises the floor to s2 + 1 and not beyond", "[list-diag][t3a1]") {
    Fx f(1);
    for (u64 page : {0x30u, 0x31u, 0x32u, 0x33u}) {
        f.gpu_modified.Add(page << 12, 64);
    }
    const auto r0 = f.Collect(0, 0x30000); // lot 1, fills the only slot
    REQUIRE(r0.registered);
    const Proto::Batch b1 = f.GatherAll(); // lot 1 taken, not applied: the slot stays published
    f.gates.Arm("w.after_collect");
    Proto::Claim claim;
    std::thread wt([&] { claim = f.Collect(0, 0x31000); });
    f.gates.WaitReached("w.after_collect");
    const Proto::Batch b2 = f.GatherAll(); // lot 2 takes the block of 0x31000 while the writer is open
    f.gates.Release("w.after_collect");
    wt.join();
    CHECK(claim.ambiguous);
    CHECK(!claim.registered); // overflow
    CHECK(f.proto.writer_shared[0].incomplete_floor.load() == 3); // s2 + 1 (s2 = 2)
    ApplyAll(f, b1); // retires r0
    ApplyAll(f, b2); // lot 2: incomplete
    // Lot 1 (r0) is <= floor: incomplete (the floor is a ceiling on lots). Lot 2 has no record for its block
    // at all (the notice overflowed): ambiguous, which has precedence.
    CHECK(f.C(f.c().blk_incomplete_floor) == 1);
    CHECK(f.C(f.c().blk_ambiguous) == 1);
    const auto c3 = f.Collect(0, 0x32000); // lot 3 <= floor: incomplete (a false "incomplete", accepted)
    CHECK(c3.lot == 3);
    ApplyAll(f, f.GatherAll());
    CHECK(f.C(f.c().blk_incomplete_floor) == 2);
    const auto c4 = f.Collect(0, 0x33000); // lot 4 > floor: complete
    CHECK(c4.lot == 4);
    ApplyAll(f, f.GatherAll());
    CHECK(f.C(f.c().blk_incomplete_floor) == 2);
    CHECK(f.C(f.c().blk_affirmed) == 1); // only lot 4: the floor does not poison the future
}

TEST_CASE("T3a.1 [5]: expiry before the application", "[list-diag][t3a1]") {
    SECTION("an unfinished record recycled: the lot is incomplete") {
        Fx f(2);
        f.gpu_modified.Add(0x23000, 64);
        f.Collect(0, 0x23000);
        f.Collect(0, 0x23010);
        const Proto::Batch b = f.GatherAll();
        g_clock += 6'000'000;
        f.Collect(0, 0x24000); // recycles the first record, not finished
        CHECK(f.W().expired == 1);
        CHECK(f.proto.writer_shared[0].incomplete_floor.load() == 1);
        ApplyAll(f, b);
        CHECK(f.C(f.c().blk_incomplete_floor) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
    }
    SECTION("a record already applied is recycled without marking") {
        Fx f(2);
        f.gpu_modified.Add(0x23000, 64);
        f.Collect(0, 0x23000);
        f.Collect(0, 0x23010);
        ApplyAll(f, f.GatherAll());
        g_clock += 6'000'000;
        f.Collect(0, 0x24000);
        CHECK(f.proto.writer_shared[0].incomplete_floor.load() == 0);
        CHECK(f.W().expired == 0);
    }
    SECTION("the application between the rise of the floor and the CAS is never complete") {
        for (const bool apply_while_blocked : {true, false}) {
            INFO("apply_while_blocked=" << apply_while_blocked);
            Fx f(2);
            f.gpu_modified.Add(0x23000, 64);
            f.Collect(0, 0x23000);
            f.Collect(0, 0x23010);
            const Proto::Batch b = f.GatherAll();
            g_clock += 6'000'000;
            f.gates.Arm("r.before_reclaim_cas");
            std::thread wt([&] { f.Collect(0, 0x24000); });
            f.gates.WaitReached("r.before_reclaim_cas");
            if (apply_while_blocked) {
                ApplyAll(f, b);
                f.gates.Release("r.before_reclaim_cas");
                wt.join();
            } else {
                f.gates.Release("r.before_reclaim_cas");
                wt.join();
                ApplyAll(f, b);
            }
            CHECK(f.C(f.c().blk_incomplete) == 1);
            CHECK(f.C(f.c().blk_affirmed) == 0);
        }
    }
}

TEST_CASE("T3a.1 [6]: the open flag, deterministic", "[list-diag][t3a1]") {
    SECTION("writer stopped after the Collect: the range is incomplete") {
        Fx f(16);
        f.gpu_modified.Add(0x24000, 64);
        f.Collect(0, 0x24000); // a record of the same block and lot
        f.gates.Arm("w.after_collect");
        std::thread wt([&] { f.Collect(0, 0x24010); });
        f.gates.WaitReached("w.after_collect");
        f.Invalidate();
        CHECK(f.C(f.c().blk_incomplete_open) == 1);
        CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
        f.gates.Release("w.after_collect");
        wt.join();
        // The second notice was ambiguous (the Gather came before its s2): its record stays pending and
        // never retires. A block it does not touch, in the next lot with no writer open, is complete.
        f.gpu_modified.Add(0x24100, 64);
        f.Collect(0, 0x24100);
        f.Invalidate();
        CHECK(f.C(f.c().blk_affirmed) == 1);
    }
    SECTION("writer stopped before O2: incomplete too") {
        Fx f(16);
        f.gpu_modified.Add(0x24000, 64);
        f.Collect(0, 0x24000);
        f.gates.Arm("w.before_open_clear");
        std::thread wt([&] { f.Collect(0, 0x24010); });
        f.gates.WaitReached("w.before_open_clear");
        f.Invalidate();
        CHECK(f.C(f.c().blk_incomplete_open) == 1);
        f.gates.Release("w.before_open_clear");
        wt.join();
    }
    SECTION("the mutant NaiveNoOpenFlag: the false completeness appears") {
        Fx f(16);
        g_no_open_flag = true;
        f.gpu_modified.Add(0x24000, 64);
        f.Collect(0, 0x24000);
        f.gates.Arm("w.after_collect");
        std::thread wt([&] { f.Collect(0, 0x24010); });
        f.gates.WaitReached("w.after_collect");
        f.Invalidate();
        CHECK(f.C(f.c().blk_affirmed) == 1); // wrongly: the second notice has no record yet
        f.gates.Release("w.after_collect");
        wt.join();
    }
}

TEST_CASE("T3a.1 [7]: late publication and a late ambiguous record", "[list-diag][t3a1]") {
    SECTION("published after the application began: late, the lot is incomplete") {
        Fx f(16);
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        f.gates.Arm("w.after_s2");
        std::thread wt([&] { f.Collect(0, 0x25010); });
        f.gates.WaitReached("w.after_s2");
        const Proto::Batch b = f.GatherAll();
        f.proto.BeginApply(b);
        f.gates.Release("w.after_s2");
        wt.join(); // registers, sees apply_started_lot >= its lot: late
        CHECK(f.W().late == 1);
        for (const Range& r : b.merged) {
            std::lock_guard lock{f.cache_mutex};
            f.proto.NoteApply(r.addr, r.size, f.gpu_modified, f.cpu_read, false, f.read_guest);
        }
        f.proto.EndApply();
        CHECK(f.C(f.c().blk_incomplete_floor) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
    }
    SECTION("ambiguous and published after the sweep: open or the floor declares it") {
        Fx f(16);
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        f.gates.Arm("w.after_collect");
        f.gates.Arm("w.after_s2");
        std::thread wt([&] { f.Collect(0, 0x25010); });
        f.gates.WaitReached("w.after_collect");
        const Proto::Batch b = f.GatherAll(); // a Gather between the Collect and s2: ambiguous
        f.gates.Release("w.after_collect");
        f.gates.WaitReached("w.after_s2");
        ApplyAll(f, b); // the sweep finds no record for the second notice
        CHECK(f.C(f.c().blk_incomplete) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
        f.gates.Release("w.after_s2");
        wt.join();
    }
}

TEST_CASE("T3a.1 [8]: a pop across selected and excluded pages", "[list-diag][t3a1]") {
    // N = 2: find four consecutive pages with exactly two in the sample.
    auto make = [](Fx& f, u64& base) {
        f.proto.sample_n = 2;
        for (base = 0x60;; ++base) {
            unsigned n = 0;
            for (u64 p = base; p < base + 4; ++p) {
                n += f.proto.Sampled(p << 12) ? 1 : 0;
            }
            if (n == 2) {
                return;
            }
        }
    };
    std::vector<u8> bytes(4 * 4096);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(i % 251);
    }
    Fx f(64), ref(64);
    u64 base = 0;
    make(f, base);
    u64 base_ref = 0;
    make(ref, base_ref);
    REQUIRE(base == base_ref);
    const u64 start = (base << 12) + 8;
    const u64 end = ((base + 4) << 12) - 8; // starts and ends inside a page
    for (Fx* x : {&f, &ref}) {
        x->read_guest = [&](u64 a, u8* out, size_t len) { std::memcpy(out, &bytes[a - (base << 12)], len); };
        for (u64 p = base; p < base + 4; ++p) {
            if (x->proto.Sampled(p << 12)) {
                x->Collect(0, (p << 12) + 16);
            }
        }
    }
    f.Pop(start, end, &bytes[8]); // ONE original pop
    CHECK(f.C(f.c().pop_ranges) == 1);
    CHECK(f.C(f.c().pop_fragments) == 4);
    CHECK(f.C(f.c().pop_fragments_selected) == 2);
    CHECK(f.C(f.c().pop_over_notice) == 1); // per original pop, not 2
    CHECK(f.C(f.c().pop_over_notice_fragments) == 2);
    CHECK(f.C(f.c().pop_over_notice_records) == 2);
    // The reference: one pop per page fragment, each with its own pointer.
    for (u64 p = base; p < base + 4; ++p) {
        const u64 lo = std::max(start, p << 12);
        const u64 hi = std::min(end, (p + 1) << 12);
        ref.Pop(lo, hi, &bytes[lo - (base << 12)]);
    }
    CHECK(f.C(f.c().pop_over_notice_records) == ref.C(ref.c().pop_over_notice_records));
    CHECK(f.C(f.c().pop_over_notice_bytes) == ref.C(ref.c().pop_over_notice_bytes));
    // The offset of the pointer: with the right bytes the guest agrees, so every sample is equal.
    f.Invalidate();
    ref.Invalidate();
    CHECK(f.C(f.c().window_sample_equal) == 2);
    CHECK(f.C(f.c().window_sample_different) == 0);
    CHECK(ref.C(ref.c().window_sample_equal) == 2);
    // A pointer that is not shifted for the second fragment is seen: the hash of the wrong bytes differs.
    Fx wrong(64);
    wrong.proto.sample_n = 2;
    wrong.read_guest = f.read_guest;
    wrong.read_guest = [&](u64 a, u8* out, size_t len) { std::memcpy(out, &bytes[a - (base << 12)], len); };
    for (u64 p = base; p < base + 4; ++p) {
        if (wrong.proto.Sampled(p << 12)) {
            wrong.Collect(0, (p << 12) + 16);
        }
    }
    wrong.Pop(start, end, &bytes[0]); // pointer of the wrong offset (8 bytes off)
    wrong.Invalidate();
    CHECK(wrong.C(wrong.c().window_sample_different) == 2);
}

TEST_CASE("T3a.1 [8b]: a pop over excluded pages only does not sweep", "[list-diag][t3a1]") {
    Fx f(64);
    f.proto.sample_n = 8;
    const u64 page = PickPage(f.proto, false);
    f.Pop(page << 12, (page << 12) + 64);
    CHECK(f.C(f.c().pop_ranges) == 1);
    CHECK(f.C(f.c().pop_fragments) == 1);
    CHECK(f.C(f.c().pop_fragments_selected) == 0);
    CHECK(f.C(f.c().pop_over_notice) == 0);
}

// ---- 9: N = 1 against the frozen T3a (with the D1 in both: they share the real manager).

namespace {

struct FrozenHooks : TestHooks {};

template <class P>
struct Script {
    explicit Script(unsigned slots) : proto(slots) {}
    std::array<Manager, kCores> managers;
    P proto;
    Common::RangeSet<u64> gpu_modified;
    Common::RangeSet<u64> cpu_read;

    template <class G, class B>
    void Round(std::mt19937& rng, std::vector<std::pair<u64, u64>>& ranges_scratch) {
        G gathered;
        proto.GatherAll(std::span<Manager>{managers}, gathered);
        ranges_scratch.clear();
        for (const auto& r : gathered.pre) {
            ranges_scratch.emplace_back(r.addr, r.size);
        }
        if (ranges_scratch.size() > 1) {
            std::sort(ranges_scratch.begin(), ranges_scratch.end());
            size_t out = 0;
            for (size_t in = 1; in < ranges_scratch.size(); ++in) {
                auto& last = ranges_scratch[out];
                const auto& [address, size] = ranges_scratch[in];
                const u64 last_end = last.first + last.second;
                if ((address >> 12) == (last.first >> 12) && address <= last_end) {
                    last.second = std::max(last_end, address + size) - last.first;
                } else {
                    ranges_scratch[++out] = ranges_scratch[in];
                }
            }
            ranges_scratch.resize(out + 1);
        }
        const B batch = proto.MakeBatch(std::move(gathered), ranges_scratch);
        proto.BeginApply(batch);
        for (const auto& r : batch.merged) {
            proto.NoteApply(r.addr, r.size, gpu_modified, cpu_read, false,
                            [](u64, u8* out, size_t len) { std::memset(out, 0xEE, len); });
        }
        proto.EndApply();
        (void)rng;
    }
};

template <class P, class G, class B>
std::map<std::string, u64> RunScript(P& proto_unused, Script<P>& sc, unsigned seed) {
    (void)proto_unused;
    std::mt19937 rng{seed};
    std::vector<std::pair<u64, u64>> scratch;
    std::array<u8, 2048> bytes{}; // a pop here spans up to ~1 KiB
    for (int step = 0; step < 4000; ++step) {
        const unsigned op = rng() % 20;
        if (op < 12) {
            const unsigned core = rng() % kCores;
            const u64 addr = 0x80000 + (rng() % 6) * 4096 + (rng() % 8) * 64 + (rng() % 16) * 4;
            sc.proto.CollectObserved(sc.managers[core], core, addr, 4);
        } else if (op < 15) {
            sc.template Round<G, B>(rng, scratch);
        } else if (op < 18) {
            const u64 page = 0x80000 + (rng() % 6) * 4096;
            sc.proto.NotePop(page + (rng() % 8) * 64, page + 512 + (rng() % 8) * 64, bytes.data());
        } else if (op == 18) {
            const u64 page = 0x80000 + (rng() % 6) * 4096;
            sc.gpu_modified.Add(page + (rng() % 8) * 64, 64);
        } else {
            g_clock += 400'000; // 0.4 s: now and then a record expires
        }
        g_clock += 7;
    }
    return {};
}

} // namespace

TEST_CASE("T3a.1 [9]: N = 1 equals the frozen T3a (both with the D1), except the new coverage counters",
          "[list-diag][t3a1]") {
    for (const unsigned seed : {1u, 2u, 3u}) {
        INFO("seed " << seed);
        for (const unsigned slots : {4u, 64u}) {
            INFO("slots " << slots);
            Script<Proto> fresh(slots);
            Script<T3aFrozen::Protocol<kCores, TestHooks>> frozen(slots);
            fresh.proto.sample_n = 1;
            fresh.proto.ledger.expiry_us = 1'500'000;
            frozen.proto.ledger.expiry_us = 1'500'000;
            g_clock = 0;
            RunScript<Proto, Proto::Gathered, Proto::Batch>(fresh.proto, fresh, seed);
            g_clock = 0;
            RunScript<T3aFrozen::Protocol<kCores, TestHooks>, T3aFrozen::Protocol<kCores, TestHooks>::Gathered,
                      T3aFrozen::Protocol<kCores, TestHooks>::Batch>(frozen.proto, frozen, seed);
            std::map<std::string, u64> now;
            for (const auto& v : fresh.proto.Snapshot()) {
                now[v.name] = v.value;
            }
            for (const auto& field : T3aFrozen::Fields(frozen.proto.counters)) {
                const std::string name = field.name;
                if (name == "blk_gpu_bytes_at_risk" || name.rfind("blk_at_risk_", 0) == 0) {
                    continue; // these move to blk_incomplete_gpu_bytes when coverage is incomplete
                }
                INFO(name);
                CHECK(now.at(name) == T3aFrozen::Get(*field.counter));
            }
            CHECK(T3aFrozen::Get(frozen.proto.counters.blk_gpu_bytes_at_risk) ==
                  now.at("blk_gpu_bytes_at_risk") + now.at("blk_incomplete_gpu_bytes"));
            // The script produced something to compare.
            CHECK(now.at("blk_applies") > 0);
            CHECK(now.at("notice_registered") > 0);
        }
    }
}

TEST_CASE("T3a.1 [10]: conservation in the sample with the execution stopped, and no false complete",
          "[list-diag][t3a1][stress]") {
    const unsigned notices = StressNotices();
    for (const u64 n : {u64{1}, u64{8}, u64{64}}) {
        INFO("N=" << n);
        for (unsigned round = 1; round <= 2; ++round) {
            const StressResult roomy = RunStress({notices, 4096, 1'000'000'000ull, round, 0, n});
            CHECK(roomy.conserved);
            CHECK(roomy.false_complete == 0);
            CHECK(roomy.bad_claims == 0);
            CHECK(roomy.missing == 0);
            const StressResult tight = RunStress({notices, 8, 400, round + 50, 0, n});
            CHECK(tight.conserved);
            CHECK(tight.false_complete == 0);
            CHECK(tight.bad_claims == 0);
        }
    }
    // The ground truth is not vacuous: without the declared coverage a false complete is seen.
    u64 seen = 0, affirmed = 0;
    for (unsigned round = 1; round <= 3; ++round) {
        const StressResult mutant = RunStress({notices, 8, 400, round + 70, 0, 1, true, false});
        seen += mutant.false_complete;
        affirmed += mutant.affirmed;
    }
    CHECK(affirmed > 0);
    CHECK(seen > 0);
}

TEST_CASE("T3a.1 [10b]: the fraction of sampled NOTICES, on distinct pages used once each", "[list-diag][t3a1]") {
    for (const u64 n : {u64{2}, u64{8}, u64{64}}) {
        INFO("N=" << n);
        Fx f(8);
        f.proto.sample_n = n;
        const u64 pages = n * 1500;
        for (u64 page = 0x1000; page < 0x1000 + pages; ++page) {
            f.Collect(0, page << 12);
            f.proto.ledger.ForEachLive([&](unsigned c, unsigned i) { // keep the small ring free
                f.proto.ledger.Free(c, f.proto.ledger.SlotAt(c, i), f.proto.ledger.SlotAt(c, i).state.load() >> 2);
            });
        }
        const double p = 1.0 / static_cast<double>(n);
        const double mean = static_cast<double>(pages) * p;
        const double sigma = std::sqrt(static_cast<double>(pages) * p * (1 - p));
        CHECK(f.W().seen == pages);
        CHECK(std::abs(static_cast<double>(f.W().sampled) - mean) < 4 * sigma);
    }
}

template <class C>
concept HasRegistered = requires(C c) { c.notice_registered; };
template <class C>
concept HasOverflow = requires(C c) { c.notice_overflow; };
template <class C>
concept HasExpired = requires(C c) { c.notice_expired; };

TEST_CASE("T3a.1 [11]: the counters of the writer are per core, 64 bytes, and not in the shared struct",
          "[list-diag][t3a1]") {
    STATIC_REQUIRE(sizeof(WriterCounters) == 64);
    STATIC_REQUIRE(alignof(WriterCounters) == 64);
    STATIC_REQUIRE(sizeof(WriterShared) == 64);
    STATIC_REQUIRE(alignof(WriterShared) == 64);
    STATIC_REQUIRE(!HasRegistered<Counters>);
    STATIC_REQUIRE(!HasOverflow<Counters>);
    STATIC_REQUIRE(!HasExpired<Counters>);
    Fx f(64);
    const u64 a = PickPage(f.proto, true);
    for (unsigned i = 0; i < 5; ++i) {
        f.Collect(1, (a << 12) + i * 64);
    }
    for (unsigned i = 0; i < 3; ++i) {
        f.Collect(2, (a << 12) + 2048 + i * 64);
    }
    CHECK(f.proto.writer_counters[1].registered.load() == 5);
    CHECK(f.proto.writer_counters[2].registered.load() == 3);
    CHECK(f.proto.writer_counters[0].registered.load() == 0);
    CHECK(f.W().registered == 8);
}

TEST_CASE("T3a.1 [12]: one clock read per Register", "[list-diag][t3a1]") {
    SECTION("a normal registration") {
        Fx f(64);
        g_clock_reads = 0;
        f.Collect(0, 0x90000);
        CHECK(g_clock_reads.load() == 1);
    }
    SECTION("an overflow with 8 published, unexpired slots (it used to be 9)") {
        Fx f(8);
        for (unsigned i = 0; i < 8; ++i) {
            CHECK(f.Collect(0, 0x90000 + i * 64).registered);
        }
        g_clock_reads = 0;
        CHECK(!f.Collect(0, 0x91000).registered);
        CHECK(g_clock_reads.load() == 1);
    }
}

TEST_CASE("T3a.1 [14]: the sample size and the seed", "[list-diag][t3a1]") {
    using namespace VideoCommon::ListDiag;
    CHECK(ParseSampleConfig(nullptr, nullptr).n == 8);
    CHECK(ParseSampleConfig("1", nullptr).n == 1);
    CHECK(ParseSampleConfig("64", nullptr).n == 64);
    CHECK(ParseSampleConfig("0x10", nullptr).n == 16);
    for (const char* bad : {"0", "abc", "-3", "", "4x", "99999999999"}) {
        const auto c = ParseSampleConfig(bad, nullptr);
        INFO(bad);
        CHECK(c.n == 8);
        CHECK(c.n_invalid);
    }
    CHECK(ParseSampleConfig(nullptr, "42").seed == 42);
    CHECK(ParseSampleConfig(nullptr, "0xff").seed == 255);
    CHECK(ParseSampleConfig(nullptr, "zz").seed_invalid);
    CHECK(ParseSampleConfig(nullptr, "zz").seed == 1);
}

TEST_CASE("T3a.1 [15]: one predicate, over the 4 KiB page, whatever the core or the hook", "[list-diag][t3a1]") {
    for (const u64 n : {u64{2}, u64{8}, u64{64}}) {
        Proto proto;
        proto.sample_n = n;
        std::mt19937_64 rng{n};
        for (int i = 0; i < 4000; ++i) {
            const u64 addr = (rng() & 0xfffffff000ull) + (rng() & 0xfff);
            // Every address of the page, in either 2 KiB half, has the verdict of the page.
            CHECK(proto.Sampled(addr) == proto.Sampled(addr & ~u64{0xfff}));
            CHECK(proto.Sampled(addr) == proto.Sampled((addr & ~u64{0xfff}) + 0x800));
        }
        // What the Gather returns for a Collect (u32(a >> 11) round trip) has the verdict of the Collect.
        Manager manager;
        std::vector<std::pair<u64, bool>> collected;
        for (int i = 0; i < 400; ++i) {
            const u64 addr = 0x100000 + (rng() % 512) * 4096 + (rng() % 64) * 64;
            manager.Collect(addr, 4);
            collected.emplace_back(addr, proto.Sampled(addr));
        }
        std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t) {
            bool found = false;
            for (const auto& [addr, verdict] : collected) {
                if (addr >> 12 == a >> 12) {
                    found = true;
                    CHECK(proto.Sampled(a) == verdict);
                }
            }
            CHECK(found);
        };
        manager.Gather(cb);
        // The seed changes the sample; the core is not an input (Sampled takes no core).
        Proto other;
        other.sample_n = n;
        other.sample_seed = 7;
        unsigned differ = 0;
        for (u64 page = 0; page < 2000; ++page) {
            differ += proto.Sampled(page << 12) != other.Sampled(page << 12) ? 1 : 0;
        }
        CHECK(differ > 0);
        // Uniform over distinct pages.
        unsigned hits = 0;
        const u64 pages = n * 2000;
        for (u64 page = 0; page < pages; ++page) {
            hits += proto.Sampled(page << 12) ? 1 : 0;
        }
        const double p = 1.0 / static_cast<double>(n);
        CHECK(std::abs(static_cast<double>(hits) - static_cast<double>(pages) * p) <
              4 * std::sqrt(static_cast<double>(pages) * p * (1 - p)));
    }
}

TEST_CASE("T3a.1 [16]: a record recycled while the application reads it", "[list-diag][t3a1]") {
    SECTION("between the two loads of state") {
        Fx f(2);
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        f.Collect(0, 0x25010);
        const Proto::Batch b = f.GatherAll();
        g_clock += 6'000'000;
        f.gates.Arm("r.between_state_loads");
        std::thread at([&] { ApplyAll(f, b); });
        f.gates.WaitReached("r.between_state_loads");
        f.Collect(0, 0x26000); // recycles the slot being read: the floor rises BEFORE the CAS
        f.gates.Release("r.between_state_loads");
        at.join();
        CHECK(f.C(f.c().observation_lost) >= 1);
        CHECK(f.C(f.c().blk_incomplete) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
    }
    SECTION("after the Read, rejected by the generation in marks") {
        Fx f(2);
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        f.Collect(0, 0x25010);
        const Proto::Batch b = f.GatherAll();
        g_clock += 6'000'000;
        f.gates.Arm("m.before_apply_cas");
        std::thread at([&] { ApplyAll(f, b); });
        f.gates.WaitReached("m.before_apply_cas");
        f.Collect(0, 0x26000);
        f.gates.Release("m.before_apply_cas");
        at.join();
        CHECK(f.C(f.c().blk_incomplete) == 1);
        CHECK(f.C(f.c().blk_affirmed) == 0);
    }
    SECTION("the mutant without the floor: the false completeness appears") {
        Fx f(2);
        g_no_coverage = true;
        f.gpu_modified.Add(0x25000, 64);
        f.Collect(0, 0x25000);
        f.Collect(0, 0x25010);
        const Proto::Batch b = f.GatherAll();
        g_clock += 6'000'000;
        f.gates.Arm("r.between_state_loads");
        std::thread at([&] { ApplyAll(f, b); });
        f.gates.WaitReached("r.between_state_loads");
        f.Collect(0, 0x26000);
        f.gates.Release("r.between_state_loads");
        at.join();
        CHECK(f.C(f.c().blk_affirmed) == 1);
    }
}

TEST_CASE("T3a.1 [17]: a Gather between the 2 KiB pieces of a sampled notice", "[list-diag][t3a1]") {
    Fx f(16);
    f.gpu_modified.Add(0x107c0, 128);
    f.gates.Arm("c.between_pieces");
    Proto::Claim c;
    std::thread wt([&] { c = f.Collect(0, 0x107c0, 128); });
    f.gates.WaitReached("c.between_pieces");
    const Proto::Batch first = f.GatherAll(); // piece 1: lot 1
    f.gates.Release("c.between_pieces");
    wt.join();
    CHECK(c.ambiguous);
    CHECK(c.registered);
    CHECK(c.handle.slot->lot.load() == 2); // the ceiling hi = s2 + 1, not a lot of the notice
    const Proto::Batch second = f.GatherAll(); // piece 2: lot 2
    ApplyAll(f, first);
    ApplyAll(f, second);
    CHECK(f.C(f.c().blk_affirmed) == 0); // neither lot affirms it
    CHECK(f.C(f.c().blk_gpu_bytes_at_risk) == 0);
}

TEST_CASE("T3a.1 [18]: an old ClearLive does not hide a new record", "[list-diag][t3a1]") {
    for (const char* point : {"l.before_clear", "l.after_clear"}) {
        INFO(point);
        Fx f(1);
        f.Collect(0, 0x27000);
        const Proto::Batch b = f.GatherAll();
        f.gates.Arm(point);
        std::thread at([&] { ApplyAll(f, b); }); // retires the record, Free -> ClearLive stops at `point`
        f.gates.WaitReached(point);
        // The slot is free (the Free CAS happened before ClearLive): a new notice reuses it.
        const auto n = f.Collect(0, 0x28000);
        CHECK(n.registered);
        f.gates.Release(point);
        at.join();
        CHECK(f.proto.ledger.LiveBits() == 1);
        unsigned visited = 0;
        f.proto.ledger.ForEachLive([&](unsigned, unsigned) { ++visited; });
        CHECK(visited == 1);
    }
}

TEST_CASE("T3a.1 [13]: diagnostic off, the writer counters do not move and nothing is allocated", "[list-diag][t3a1]") {
    const u32 saved = g_parts.load();
    g_parts = 0;
    const u64 before = g_state.load() != nullptr ? State().Sums().seen : 0;
    Manager manager;
    CollectNotice(manager, 0, 0x30000, 4); // CollectT<false>: the flag is read and nothing else
    const u64 after = g_state.load() != nullptr ? State().Sums().seen : 0;
    CHECK(after == before);
    g_parts = saved;
}

TEST_CASE("T3a.1b: after an overflow the cursor skips the slots examined, so the next notice registers", "[list-diag][t3a1b]") {
    Fx f(16);
    // 8 records of lot 1 (slots 0-7), 8 of lot 2 (slots 8-15): the ring is full and the cursor is back at 0.
    for (unsigned i = 0; i < 8; ++i) {
        REQUIRE(f.Collect(0, 0xA0000 + i * 4096).registered);
    }
    const Proto::Batch b1 = f.GatherAll(); // lot 1 taken, never applied: slots 0-7 stay published
    for (unsigned i = 8; i < 16; ++i) {
        REQUIRE(f.Collect(0, 0xA0000 + i * 4096).registered);
    }
    const Proto::Batch b2 = f.GatherAll();
    ApplyAll(f, b2); // retires slots 8-15 (lot 2)
    (void)b1;
    CHECK(f.proto.ledger.LiveBits() == 8);
    // The 8 slots from the cursor are published and not expired; the rest is free.
    const auto first = f.Collect(0, 0xB0000);
    CHECK(!first.registered);
    CHECK(f.W().overflow == 1);
    const auto second = f.Collect(0, 0xB1000);
    CHECK(second.registered); // outside the 8 examined before
    CHECK(f.W().overflow == 1);
    CHECK(f.W().registered == 17);
    CHECK(f.Conserved());
}

// =============================================================================================
// T3a.2 blkcmp (docs/list-diag-design.md, 3.8): what the harness can test. The copy in Vulkan, the Finish, the
// capture in InlineMemoryImplementation and the BufferCache as a whole are NOT here (3.8.8).

namespace {

/// One affirmed range with at_risk bytes on a fresh page; the request left pending for this thread (or null).
const Proto::BlkcmpRequest* AffirmedCycle(Fx& f, u64 page, u32 exact_off = 0, u32 exact_size = 4) {
    f.gpu_modified.Add(page << 12, 64);
    f.Collect(0, (page << 12) + exact_off, exact_size);
    ApplyAll(f, f.GatherAll());
    return f.proto.BlkcmpPending(page << 12, 64);
}

struct BlkcmpFx {
    Fx f{64};
    BlkcmpFx() {
        f.proto.blkcmp_on = true;
        f.proto.BlkcmpExpire(); // nothing left from an earlier test of this thread
    }
    ~BlkcmpFx() {
        f.proto.BlkcmpExpire();
    }
};

} // namespace

TEST_CASE("blkcmp [1]: the draw is reproducible, uses its own seed and is 1/M", "[list-diag][blkcmp]") {
    constexpr unsigned kRanges = 3000;
    const auto run = [&](u64 seed, u64 odds) {
        BlkcmpFx b;
        b.f.proto.blkcmp_seed = seed;
        b.f.proto.blkcmp_odds = odds;
        b.f.proto.blkcmp_max_bytes = 4096;
        std::vector<bool> drawn;
        for (unsigned i = 0; i < kRanges; ++i) {
            drawn.push_back(AffirmedCycle(b.f, 0xC000 + i) != nullptr);
            b.f.proto.BlkcmpExpire();
        }
        CHECK(b.f.C(b.f.c().blkcmp_eligible) == kRanges);
        return drawn;
    };
    const auto a = run(123, 4);
    const auto again = run(123, 4);
    const auto other = run(124, 4);
    CHECK(a == again); // same seed, same sequence: same draw
    CHECK(a != other); // another seed: another draw
    // It is the formula: eligible number k is drawn iff SplitMix64(seed ^ k) % M == 0.
    for (unsigned k = 0; k < kRanges; ++k) {
        REQUIRE(static_cast<bool>(a[k]) == (Proto::SplitMix64(123 ^ k) % 4 == 0));
    }
    const double n = static_cast<double>(std::count(a.begin(), a.end(), true));
    const double mean = kRanges / 4.0;
    const double sigma = std::sqrt(kRanges * 0.25 * 0.75);
    CHECK(std::abs(n - mean) < 4 * sigma);
    // M = 1 draws everything.
    const auto all = run(5, 1);
    CHECK(std::count(all.begin(), all.end(), true) == kRanges);
}

TEST_CASE("blkcmp [2]: the bucket, sustained rate and burst", "[list-diag][blkcmp]") {
    BlkcmpFx b;
    b.f.proto.blkcmp_odds = 1;
    b.f.proto.blkcmp_rate = 5;
    b.f.proto.blkcmp_burst = 5;
    unsigned page = 0xD000;
    const auto take = [&] {
        REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
        const bool ok = b.f.proto.BlkcmpTakeToken();
        b.f.proto.BlkcmpExpire();
        return ok;
    };
    g_clock = 1'000'000;
    for (int i = 0; i < 5; ++i) {
        CHECK(take()); // the burst: 5 at once
    }
    CHECK(!take()); // empty
    CHECK(b.f.C(b.f.c().blkcmp_discard_budget) == 1);
    g_clock += 200'000; // 0.2 s at 5/s: exactly one token
    CHECK(take());
    CHECK(!take());
    g_clock += 100'000; // half a token
    CHECK(!take());
    g_clock += 100'000; // together, one
    CHECK(take());
    g_clock += 60'000'000; // a long idle: the bucket holds the burst, no more
    for (int i = 0; i < 5; ++i) {
        CHECK(take());
    }
    CHECK(!take());
    CHECK(b.f.C(b.f.c().blkcmp_discard_budget) == 4);
}

TEST_CASE("blkcmp [3]: the size cap and the number of parts", "[list-diag][blkcmp]") {
    SECTION("the cap on the at_risk bytes") {
        BlkcmpFx b;
        b.f.proto.blkcmp_odds = 1;
        b.f.proto.blkcmp_max_bytes = 32; // a 64-byte block with a 4-byte write has 60 at risk
        CHECK(AffirmedCycle(b.f, 0xE000) == nullptr);
        CHECK(b.f.C(b.f.c().blkcmp_discard_size) == 1);
        b.f.proto.blkcmp_max_bytes = 60;
        CHECK(AffirmedCycle(b.f, 0xE001) != nullptr); // exactly at the cap is accepted
    }
    SECTION("more parts than a request holds is discarded, not truncated") {
        BlkcmpFx b;
        b.f.proto.blkcmp_odds = 1;
        // 17 separate GPU pieces in one block, no exact write in them (the write is in the one byte left).
        const u64 page = 0xE100;
        for (unsigned i = 0; i < 17; ++i) {
            b.f.gpu_modified.Add((page << 12) + i * 3, 1);
        }
        b.f.Collect(0, (page << 12) + 60, 4);
        ApplyAll(b.f, b.f.GatherAll());
        CHECK(b.f.C(b.f.c().blkcmp_discard_ranges) == 1);
        CHECK(b.f.proto.BlkcmpPending(page << 12, 64) == nullptr);
    }
}

TEST_CASE("blkcmp [4]: the at_risk bytes of the request and the layout of the staging", "[list-diag][blkcmp]") {
    BlkcmpFx b;
    b.f.proto.blkcmp_odds = 1;
    // A 64-byte block, GPU bytes everywhere, the CPU wrote 4 bytes at +16: at risk are [0,16) and [20,64).
    const u64 page = 0xE200;
    const auto* request = AffirmedCycle(b.f, page, 16, 4);
    REQUIRE(request != nullptr);
    CHECK(request->addr == page << 12);
    CHECK(request->size == 64);
    REQUIRE(request->count == 2);
    CHECK(request->parts[0].addr == (page << 12) + 0);
    CHECK(request->parts[0].size == 16);
    CHECK(request->parts[1].addr == (page << 12) + 20);
    CHECK(request->parts[1].size == 44);
    CHECK(request->total == 60);
    CHECK(request->mask == 1u);
    std::array<u64, kBlkcmpParts> offsets{};
    const u64 total = Proto::BlkcmpLayout(*request, offsets);
    CHECK(offsets[0] == 0);
    CHECK(offsets[1] == 64); // each part aligned to 64 bytes
    CHECK(total == 128);
}

TEST_CASE("blkcmp [5]: the comparison with a fake staging", "[list-diag][blkcmp]") {
    const auto run = [&](const std::function<void(std::vector<u8>&)>& tweak, u64 expect_diff, u64 expect_first) {
        BlkcmpFx b;
        b.f.proto.blkcmp_odds = 1;
        const u64 page = 0xE300;
        const auto* request = AffirmedCycle(b.f, page, 16, 4);
        REQUIRE(request != nullptr);
        std::vector<u8> payload(64);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<u8>(i + 1);
        }
        std::array<u64, kBlkcmpParts> offsets{};
        const u64 total = Proto::BlkcmpLayout(*request, offsets);
        std::vector<u8> staging(total, 0xAA);
        for (unsigned i = 0; i < request->count; ++i) { // the old bytes equal the payload
            for (u64 k = 0; k < request->parts[i].size; ++k) {
                staging[offsets[i] + k] = payload[request->parts[i].addr - request->addr + k];
            }
        }
        tweak(staging);
        MarkThread(ThreadRole::Back);
        const bool different = b.f.proto.BlkcmpCompare(staging.data(), offsets, payload, 1234);
        MarkThread(ThreadRole::Unknown);
        CHECK(different == (expect_diff != 0));
        CHECK(b.f.C(b.f.c().blkcmp_bytes_compared) == 60); // only the at_risk bytes
        CHECK(b.f.C(b.f.c().blkcmp_bytes_different) == expect_diff);
        CHECK(b.f.C(b.f.c().blkcmp_compared) == 1);
        CHECK(b.f.C(b.f.c().blkcmp_compared_back) == 1);
        CHECK(b.f.C(b.f.c().blkcmp_equal) == (expect_diff == 0 ? 1u : 0u));
        CHECK(b.f.C(b.f.c().blkcmp_different) == (expect_diff == 0 ? 0u : 1u));
        CHECK(b.f.C(b.f.c().blkcmp_finish_us_max) == 1234);
        CHECK(b.f.proto.BlkcmpPending(page << 12, 64) == nullptr); // the request ended
        if (expect_diff != 0) {
            const auto lines = b.f.Lines();
            REQUIRE(!lines.empty());
            char want[40];
            std::snprintf(want, sizeof(want), "first=0x%llx", static_cast<unsigned long long>((page << 12) + expect_first));
            bool found = false;
            for (const auto& line : lines) {
                found = found || (line.find("[blkcmp] different") != std::string::npos && line.find(want) != std::string::npos);
            }
            CHECK(found);
        }
    };
    run([](std::vector<u8>&) {}, 0, 0);
    run([](std::vector<u8>& s) { s[0] ^= 0xFF; }, 1, 0);          // first byte of the first part
    run([](std::vector<u8>& s) { s[64 + 43] ^= 0x01; }, 1, 20 + 43); // last byte of the last part
    run([](std::vector<u8>& s) { s[3] ^= 1; s[64] ^= 1; s[70] ^= 1; }, 3, 3);
    // Bytes outside the parts (the padding of the alignment) are not compared.
    run([](std::vector<u8>& s) { s[16] ^= 0xFF; s[63] ^= 0xFF; s[64 + 44] ^= 0xFF; }, 0, 0);
}

TEST_CASE("blkcmp [6]: the discards by reason and the conservation", "[list-diag][blkcmp]") {
    BlkcmpFx b;
    b.f.proto.blkcmp_odds = 1;
    b.f.proto.blkcmp_burst = 100;
    unsigned page = 0xE400;
    std::array<u64, kBlkcmpParts> offsets{};
    // no upload: the scope ends with the request pending
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    b.f.proto.BlkcmpExpire();
    CHECK(b.f.C(b.f.c().blkcmp_discard_no_upload) == 1);
    // stale: asked for another range
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    CHECK(b.f.proto.BlkcmpPending(0x1234000, 64) == nullptr);
    CHECK(b.f.C(b.f.c().blkcmp_discard_stale) == 1);
    CHECK(b.f.proto.BlkcmpPending(0x1234000, 64) == nullptr); // nothing left
    // no memory maps, no single buffer
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    b.f.proto.BlkcmpDiscard(BlkcmpReason::NoMemoryMaps);
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    b.f.proto.BlkcmpDiscard(BlkcmpReason::NoSingleBuffer);
    CHECK(b.f.C(b.f.c().blkcmp_discard_no_memory_maps) == 1);
    CHECK(b.f.C(b.f.c().blkcmp_discard_no_single_buffer) == 1);
    // two compared, one equal and one different
    for (const bool diff : {false, true}) {
        const auto* request = AffirmedCycle(b.f, page++);
        REQUIRE(request != nullptr);
        std::vector<u8> payload(64, 7);
        std::vector<u8> staging(Proto::BlkcmpLayout(*request, offsets), 7);
        if (diff) {
            staging[0] = 8;
        }
        b.f.proto.BlkcmpCompare(staging.data(), offsets, payload, 10);
    }
    // the size cap, and the budget
    b.f.proto.blkcmp_max_bytes = 8;
    CHECK(AffirmedCycle(b.f, page++) == nullptr);
    b.f.proto.blkcmp_max_bytes = 4096;
    b.f.proto.blkcmp_burst = 1;
    b.f.proto.blkcmp_rate = 1;
    g_clock += 10'000'000;
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    CHECK(b.f.proto.BlkcmpTakeToken());
    b.f.proto.BlkcmpExpire(); // the request is still pending after a good token: no upload
    REQUIRE(AffirmedCycle(b.f, page++) != nullptr);
    CHECK(!b.f.proto.BlkcmpTakeToken());
    // drawn = compared + every discard
    const auto& c = b.f.c();
    const u64 discards = Get(c.blkcmp_discard_ranges) + Get(c.blkcmp_discard_size) + Get(c.blkcmp_discard_no_upload) +
                         Get(c.blkcmp_discard_no_memory_maps) + Get(c.blkcmp_discard_no_single_buffer) +
                         Get(c.blkcmp_discard_budget) + Get(c.blkcmp_discard_stale);
    CHECK(Get(c.blkcmp_drawn) == Get(c.blkcmp_compared) + discards);
    CHECK(Get(c.blkcmp_compared) == Get(c.blkcmp_equal) + Get(c.blkcmp_different));
    CHECK(Get(c.blkcmp_eligible) >= Get(c.blkcmp_drawn));
    // the summary says what it is
    const auto summary = b.f.proto.SummaryLines();
    bool text = false, caveat = false, random = false;
    for (const auto& line : summary) {
        text = text || line.find("bytes antigos da GPU diferentes do payload que os substituiu") != std::string::npos;
        caveat = caveat || line.find("nao prova corrupcao visivel nem exclui uma escrita legitima concorrente") != std::string::npos;
        random = random || line.find("NAO sao amostra aleatoria") != std::string::npos;
    }
    CHECK(text);
    CHECK(caveat);
    CHECK(random);
}

TEST_CASE("blkcmp [7]: nothing without blk or with blkcmp off, and the other counters do not change", "[list-diag][blkcmp]") {
    SECTION("blkcmp off: no request, no counter") {
        Fx f(64);
        f.proto.BlkcmpExpire();
        CHECK(AffirmedCycle(f, 0xE500) == nullptr);
        for (const FieldRef& field : Fields(f.c())) {
            if (std::string{field.name}.rfind("blkcmp_", 0) == 0) {
                INFO(field.name);
                CHECK(Get(*field.counter) == 0);
            }
        }
    }
    SECTION("blkcmp on but blk off: AccountBlock does not run") {
        Fx f(64);
        f.proto.blkcmp_on = true;
        f.proto.blk_on = false;
        f.proto.blkcmp_odds = 1;
        CHECK(AffirmedCycle(f, 0xE501) == nullptr);
        CHECK(f.C(f.c().blkcmp_eligible) == 0);
    }
    SECTION("the same script with and without blkcmp gives the same blk and notice counters") {
        std::map<std::string, u64> with, without;
        for (const bool on : {true, false}) {
            Script<Proto> sc(16);
            sc.proto.blkcmp_on = on;
            sc.proto.blkcmp_odds = 2;
            sc.proto.ledger.expiry_us = 1'500'000;
            g_clock = 0;
            RunScript<Proto, Proto::Gathered, Proto::Batch>(sc.proto, sc, 7);
            sc.proto.BlkcmpExpire();
            auto& target = on ? with : without;
            for (const auto& v : sc.proto.Snapshot()) {
                target[v.name] = v.value;
            }
        }
        unsigned compared = 0;
        for (const auto& [name, value] : without) {
            if (name.rfind("blkcmp_", 0) == 0) {
                continue;
            }
            INFO(name);
            CHECK(with.at(name) == value);
            ++compared;
        }
        CHECK(compared > 20);
        CHECK(with.at("blkcmp_eligible") > 0);
    }
}

TEST_CASE("blkcmp [8]: the part, its configuration and the thread mark", "[list-diag][blkcmp]") {
    using namespace VideoCommon::ListDiag;
    CHECK((ParseParts("blkcmp") & PartBlkcmp) != 0);
    CHECK((ParseParts("pop,blk,blkcmp") & (PartPop | PartBlk | PartBlkcmp)) == (PartPop | PartBlk | PartBlkcmp));
    CHECK((ParseParts("1") & PartBlkcmp) == 0); // never in "1": it perturbs
    const auto d = ParseBlkcmpConfig(1, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(d.odds == 4);
    CHECK(d.rate == 5);
    CHECK(d.burst == 5);
    CHECK(d.max_bytes == 4096);
    CHECK(d.invalid.empty());
    CHECK(d.seed == Proto::SplitMix64(1 ^ 0x626c6b636d70ull)); // derived from the sample seed
    CHECK(ParseBlkcmpConfig(2, nullptr, nullptr, nullptr, nullptr, nullptr).seed != d.seed);
    const auto over = ParseBlkcmpConfig(1, "99", "2", "10", "3", "1024");
    CHECK(over.seed == 99);
    CHECK(over.odds == 2);
    CHECK(over.rate == 10);
    CHECK(over.burst == 3);
    CHECK(over.max_bytes == 1024);
    const auto bad = ParseBlkcmpConfig(1, "x", "0", "-1", "", "9999999999");
    CHECK(bad.odds == 4);
    CHECK(bad.max_bytes == 4096);
    CHECK(bad.invalid == "SEED,ODDS,BUDGET,BURST,MAXBYTES");
    // The mark: unmarked is other/unknown, never the front.
    CHECK(t_role == ThreadRole::Unknown);
    BlkcmpFx b;
    b.f.proto.blkcmp_odds = 1;
    const auto compare_with = [&](ThreadRole role) {
        const auto* request = AffirmedCycle(b.f, 0xE600 + static_cast<unsigned>(role) * 7 + b.f.C(b.f.c().blkcmp_compared));
        REQUIRE(request != nullptr);
        std::array<u64, kBlkcmpParts> offsets{};
        std::vector<u8> payload(64, 1), staging(Proto::BlkcmpLayout(*request, offsets), 1);
        MarkThread(role);
        b.f.proto.BlkcmpCompare(staging.data(), offsets, payload, 1);
        MarkThread(ThreadRole::Unknown);
    };
    compare_with(ThreadRole::Unknown);
    compare_with(ThreadRole::Back);
    compare_with(ThreadRole::Front);
    compare_with(ThreadRole::Unknown);
    CHECK(b.f.C(b.f.c().blkcmp_compared_other) == 2);
    CHECK(b.f.C(b.f.c().blkcmp_compared_back) == 1);
    CHECK(b.f.C(b.f.c().blkcmp_compared_front) == 1);
    // A thread does not see the request of another.
    const auto* mine = AffirmedCycle(b.f, 0xE700);
    REQUIRE(mine != nullptr);
    bool other_sees = true;
    std::thread t([&] { other_sees = b.f.proto.BlkcmpPending(0xE700 << 12, 64) != nullptr; });
    t.join();
    CHECK(!other_sees);
}

// =============================================================================================
// [t4] Directed reproduction of problem B (docs/t4-coherence-fix-plan.md, 1 and 5): a held CPU write applied
// at the 64-byte grain puts the guest's OLD values over bytes only the GPU wrote.
//
// The model, deliberately small: the REAL GPUDirtyMemoryManager, a Common::RangeSet as gpu_modified_ranges and
// two byte vectors of one 4 KiB page, the host (what the GPU buffer holds) and the guest (what the CPU sees).
// The "application" does what InlineMemoryImplementation does for each range the Gather delivers: subtract it
// from gpu_modified and copy those bytes from the guest to the host.
//
// What these tests do NOT cover: BufferCache<P>, SynchronizeBuffer, the staging and the Vulkan copies, and
// problems A (an old download published over a write not yet applied) and M (a notice published before the
// memcpy finished). They keep their own cases (list diag [pop], 3.5; tools/gpu-contract-harness); this model
// has no downloads and no concurrent memcpy, so it cannot show them.
//
// Every scenario has two cases. (a), WITHOUT the tag, assembles the script through the same function and checks
// the preparation (the bytes written, the range the Gather delivers, the state of gpu_modified): an accident in
// the assembly fails there and is never a successful reproduction. (b), WITH [!shouldfail], uses the same
// assembly and makes only the assertion of the loss that is expected to be absent after the fix. Today (b)
// fails, as the tag demands. When T4 fixes the problem (b) passes, the tag then ACCUSES (Catch2 reports a
// [!shouldfail] test that passes as a failure), and whoever fixes it must remove the tag and keep the assertion.

namespace {

constexpr u64 kPageBase = 0x20000;
constexpr u8 kOldGuest = 0x11; ///< what the guest holds for everything the CPU did not write (stale for GPU bytes)
constexpr u8 kGpuByte = 0x77;  ///< what the GPU wrote into the host
constexpr u8 kCpuByte = 0x99;  ///< what the CPU wrote
constexpr u8 kGpuByteTail = 0x66; ///< what the GPU wrote into [40, 48) in the residual scenarios

struct T4World {
    std::array<u8, 4096> host{};  ///< the buffer on the GPU
    std::array<u8, 4096> guest{}; ///< guest memory
    Common::RangeSet<u64> gpu_modified;
    Manager manager;
    std::vector<std::pair<u64, u64>> delivered; ///< the ranges of the Gather, in order
    std::array<u8, 4096> host_before_apply{};
    bool gpu_range_present_before = false;
    bool gpu_range_empty_after = false;

    T4World() {
        host.fill(kOldGuest);
        guest.fill(kOldGuest);
    }
    /// The GPU wrote [begin, end) of the host and marked it GPU-modified; the guest keeps the old values.
    void GpuWrites(u64 begin, u64 end) {
        std::fill(host.begin() + begin, host.begin() + end, kGpuByte);
        gpu_modified.Add(kPageBase + begin, end - begin);
    }
    /// The CPU writes the guest and the held write is collected.
    void CpuWrites(u64 offset, u64 size) {
        std::fill(guest.begin() + offset, guest.begin() + offset + size, kCpuByte);
        manager.Collect(kPageBase + offset, size);
    }
    /// Gather + application, as InlineMemoryImplementation does for each range.
    void Apply() {
        host_before_apply = host;
        gpu_range_present_before = false;
        gpu_modified.ForEachInRange(kPageBase, 4096, [&](u64, u64) { gpu_range_present_before = true; });
        delivered.clear();
        std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { delivered.emplace_back(a, s); };
        manager.Gather(cb);
        for (const auto& [a, s] : delivered) {
            gpu_modified.Subtract(a, s);
            std::memcpy(&host[a - kPageBase], &guest[a - kPageBase], s);
        }
        bool any = false;
        gpu_modified.ForEachInRange(kPageBase, 4096, [&](u64, u64) { any = true; });
        gpu_range_empty_after = !any;
    }
    /// C1: only the bytes the CPU wrote changed in the host, and they hold the CPU's values.
    [[nodiscard]] bool OnlyCpuBytesChanged(u64 offset, u64 size) const {
        for (u64 i = 0; i < 4096; ++i) {
            const bool written = i >= offset && i < offset + size;
            if (written ? host[i] != kCpuByte : host[i] != host_before_apply[i]) {
                return false;
            }
        }
        return true;
    }
};

/// The script of every scenario: the GPU writes [0, 32), the CPU writes [offset, offset + size). With
/// `divergent_tail` the GPU also wrote [40, 48) with values the guest does NOT have (host != guest there), which
/// the residual scenarios need: a write of [41, 45) or [41, 42) makes a grain of 4 or 8 bytes take the
/// neighbours [40, 48) or [40, 44), and if host and guest began equal there the loss would be invisible.
std::unique_ptr<T4World> AssembleB(u64 offset, u64 size, bool divergent_tail = false) { // the manager is not movable
    auto w = std::make_unique<T4World>();
    w->GpuWrites(0, 32);
    if (divergent_tail) {
        std::fill(w->host.begin() + 40, w->host.begin() + 48, kGpuByteTail);
        w->gpu_modified.Add(kPageBase + 40, 8);
    }
    w->CpuWrites(offset, size);
    w->Apply();
    return w;
}

/// The preparation, common to (a) of each scenario. `lost` says the current behaviour: the host lost [0, 32).
void CheckPreparation(const T4World& w, u64 offset, u64 size, bool divergent_tail = false) {
    // The bytes written: the guest holds the CPU's bytes and the old values elsewhere.
    for (u64 i = 0; i < 64; ++i) {
        const bool written = i >= offset && i < offset + size;
        REQUIRE(w.guest[i] == (written ? kCpuByte : kOldGuest));
    }
    // The GPU's bytes were in the host before the application and gpu_modified held them.
    for (u64 i = 0; i < 32; ++i) {
        REQUIRE(w.host_before_apply[i] == kGpuByte);
    }
    REQUIRE(w.gpu_range_present_before);
    // The Gather delivers the whole 64-byte block (the grain of the manager), and the application subtracted it.
    REQUIRE(w.delivered.size() == 1);
    REQUIRE(w.delivered[0] == std::pair<u64, u64>{kPageBase, 64});
    REQUIRE(w.gpu_range_empty_after);
    // The CPU's bytes arrived.
    for (u64 i = offset; i < offset + size; ++i) {
        REQUIRE(w.host[i] == kCpuByte);
    }
    // The current behaviour, the defect: the GPU's [0, 32) came back as the guest's old values.
    for (u64 i = 0; i < 32; ++i) {
        REQUIRE(w.host[i] == kOldGuest);
    }
    if (divergent_tail) {
        // The divergence was built: the host held the GPU's values in [40, 48) and the guest did not have them
        // (outside the CPU's own bytes); and the GPU's neighbours of the write are the ones lost today.
        for (u64 i = 40; i < 48; ++i) {
            REQUIRE(w.host_before_apply[i] == kGpuByteTail);
            const bool written = i >= offset && i < offset + size;
            REQUIRE(w.guest[i] == (written ? kCpuByte : kOldGuest));
            REQUIRE(w.guest[i] != w.host_before_apply[i]);
            if (!written) {
                REQUIRE(w.host[i] == kOldGuest); // lost at the 64-byte grain
            }
        }
    }
}

} // namespace

TEST_CASE("t4 [a]: B, the main script: the Gather gives [0,64) and the host loses [0,32) (today)", "[list-diag][t4]") {
    const auto w = AssembleB(40, 4);
    CheckPreparation(*w, 40, 4);
}

// Fails today (the loss is real). Remove [!shouldfail] when T4 fixes the problem: this case then passes, and the
// tag makes Catch2 report that as a failure, which is the signal to remove it.
TEST_CASE("t4 [b]: B, the main script: C1, the host keeps [0,32) and receives [40,44)", "[list-diag][t4][!shouldfail]") {
    const auto w = AssembleB(40, 4);
    CHECK(w->OnlyCpuBytesChanged(40, 4));
}

// The residual. With the manager at 64 B this only shows the current defect. It will show the residual of O1
// (a write smaller than or misaligned to the grain still takes its neighbours) only when it runs with the
// candidate grain, after O1 exists.
// The GPU's data is divergent in [0, 32) AND [40, 48) here (revision 2 of the T4 plan), so the bytes outside the
// exact write but inside the rounding to a grain of 4 or 8 bytes ([40, 48) and [40, 44)) show the loss.
TEST_CASE("t4 [a]: residual, a misaligned 4-byte write [41,45) and a 1-byte write", "[list-diag][t4]") {
    CheckPreparation(*AssembleB(41, 4, true), 41, 4, true);
    CheckPreparation(*AssembleB(41, 1, true), 41, 1, true);
}

TEST_CASE("t4 [b]: residual, a misaligned 4-byte write [41,45): C1", "[list-diag][t4][!shouldfail]") {
    const auto w = AssembleB(41, 4, true);
    CHECK(w->OnlyCpuBytesChanged(41, 4));
}

TEST_CASE("t4 [b]: residual, a 1-byte write at 41: C1", "[list-diag][t4][!shouldfail]") {
    const auto w = AssembleB(41, 1, true);
    CHECK(w->OnlyCpuBytesChanged(41, 1));
}

// Two runs of bits in one transformation (bits 0-3 and 8 of the mask). This documents the CURRENT behaviour: both
// ranges come out. It does NOT show the defect of the Gather loop (gpu_dirty_memory_manager.h:195,
// "continuous_bits < align_size") with a smaller grain: with 64 B the mask has 32 bits, the condition is always
// true and nothing is lost. The case has to run again with the candidate grain when O1 is implemented. The
// manager is not changed here.
TEST_CASE("t4: two runs of bits in one transformation give both ranges (current behaviour)", "[list-diag][t4]") {
    // The addresses come from the GRAIN, not from literals: bits 0-3 of the mask are the first four grains and
    // bit 8 is the ninth, in the SAME transformation (32 grains) whatever the grain is. kTestGrain is the grain of
    // the manager today (its align_size, private); when O1 exists it becomes the candidate grain (4 or 8) and the
    // case then runs with the smaller grain, where the Gather loop condition "continuous_bits < align_size"
    // (gpu_dirty_memory_manager.h:195) is what must be fixed. The base is aligned to the transformation.
    constexpr u64 kTestGrain = 64;
    constexpr u64 kTransformation = 32 * kTestGrain;
    static_assert(kPageBase % kTransformation == 0);
    Manager manager;
    manager.Collect(kPageBase, 4 * kTestGrain);                       // bits 0-3
    manager.Collect(kPageBase + 8 * kTestGrain, kTestGrain);          // bit 8
    std::vector<std::pair<u64, u64>> got;
    std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { got.emplace_back(a, s); };
    manager.Gather(cb);
    REQUIRE(got.size() == 2);
    CHECK(got[0] == std::pair<u64, u64>{kPageBase, 4 * kTestGrain});
    CHECK(got[1] == std::pair<u64, u64>{kPageBase + 8 * kTestGrain, kTestGrain});
}

// =============================================================================================
// T4.0 (docs/t4-coherence-fix-plan.md, 3 and 8, item 4): the empty transformation is the one with mask 0, found by
// a predicate that takes the TRANSFORMATION. The observable output of the Gather (order, ranges, no empty
// callback) is the same as the manager of before for the same serial sequence of operations; the difference is
// internal: the empty one is no longer queued in back_buffer and front_buffer.

namespace {

using Out = std::vector<std::pair<u64, u64>>;

Out GatherOf(Manager& m) {
    Out out;
    std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { out.emplace_back(a, s); };
    m.Gather(cb);
    return out;
}
Out GatherOf(Legacy::GPUDirtyMemoryManager& m) {
    Out out;
    std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { out.emplace_back(a, s); };
    m.Gather(cb);
    return out;
}

} // namespace

TEST_CASE("t4.0 [1]: empty, collect, Gather, empty Gather, collect again (reuse)", "[list-diag][t40]") {
    Manager m;
    Legacy::GPUDirtyMemoryManager legacy;
    CHECK(GatherOf(m).empty()); // empty at the start
    CHECK(GatherOf(legacy).empty());
    for (const u64 a : {u64{0x10040}, u64{0x20000}, u64{0x10040}}) {
        m.Collect(a, 64);
        legacy.Collect(a, 64);
        const Out got = GatherOf(m);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == std::pair<u64, u64>{a, 64});
        CHECK(got == GatherOf(legacy));
        CHECK(GatherOf(m).empty()); // the empty Gather after it
        CHECK(GatherOf(legacy).empty());
    }
}

TEST_CASE("t4.0 [2]: empty, collect in A, collect in B: the change of address does not lose A", "[list-diag][t40]") {
    Manager m;
    Legacy::GPUDirtyMemoryManager legacy;
    m.Collect(0x30000, 128);
    m.Collect(0x31000, 64); // another 2 KiB transformation: A goes to back_buffer
    legacy.Collect(0x30000, 128);
    legacy.Collect(0x31000, 64);
    const Out got = GatherOf(m);
    REQUIRE(got.size() == 2);
    CHECK(got[0] == std::pair<u64, u64>{0x30000, 128});
    CHECK(got[1] == std::pair<u64, u64>{0x31000, 64});
    CHECK(got == GatherOf(legacy));
    // After a Gather the manager is empty: the first collect at a different address must not queue an empty one.
    m.Collect(0x32000, 64);
    m.Collect(0x30800, 64);
    legacy.Collect(0x32000, 64);
    legacy.Collect(0x30800, 64);
    CHECK(GatherOf(m) == GatherOf(legacy));
}

TEST_CASE("t4.0 [3]: a non-zero mask at the address of the marker is a write, not the empty one", "[list-diag][t40]") {
    // WHAT THIS EXERCISES: the REPRESENTATION. The u32 address field equal to 0xffffffff (the value of the empty
    // marker, default_transform) with a non-zero mask. With the transformation of today (2 KiB), that field is the
    // PAddr 0xffffffff << 11 = 0x7ffffffff800, far above the 2^39 of the real address space, so this is NOT the
    // limit of the address space. With a 4-byte grain (a 128-byte transformation, T4 O1) the same field value would
    // be a different, lower address, which is the case that matters; this test is the guard for that day. A test of
    // the address (`address != ~0U`) would discard this write; the test of the mask does not.
    constexpr u64 kMarkerPage = u64{0xffffffff} << 11;
    Manager m;
    Legacy::GPUDirtyMemoryManager legacy;
    m.Collect(kMarkerPage + 64, 64);
    legacy.Collect(kMarkerPage + 64, 64);
    const Out got = GatherOf(m);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == std::pair<u64, u64>{kMarkerPage + 64, 64});
    CHECK(got == GatherOf(legacy));
    // The same page with the manager holding something else first, and then the marker page swapped out.
    m.Collect(0x40000, 64);
    m.Collect(kMarkerPage, 64);
    m.Collect(0x41000, 64);
    legacy.Collect(0x40000, 64);
    legacy.Collect(kMarkerPage, 64);
    legacy.Collect(0x41000, 64);
    const Out both = GatherOf(m);
    CHECK(both.size() == 3);
    CHECK(both == GatherOf(legacy));
    // A write at the marker page right after a Gather (current is the empty marker with the SAME address field).
    m.Collect(kMarkerPage + 128, 64);
    m.Collect(kMarkerPage + 256, 64);
    const Out merged = GatherOf(m);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0] == std::pair<u64, u64>{kMarkerPage + 128, 64});
    CHECK(merged[1] == std::pair<u64, u64>{kMarkerPage + 256, 64});
}

TEST_CASE("t4.0 [4]: random sequences with many Gathers: the same output as the manager of before", "[list-diag][t40]") {
    Legacy::GPUDirtyMemoryManager legacy;
    Manager plain;
    Manager diagnostic;
    Core::GatherLotState lots;
    std::mt19937 rng{2026};
    bool same = true;
    for (int step = 0; step < 60000; ++step) {
        if (rng() % 2 == 0) { // half of the steps are Gathers: most find the empty marker
            Out a = GatherOf(legacy), b = GatherOf(plain), c;
            std::function<void(PAddr, size_t)> cb = [&](PAddr x, size_t y) { c.emplace_back(x, y); };
            u64 lot = 0;
            diagnostic.GatherT<true, TestHooks>(cb, &lots, &lot);
            same = same && a == b && a == c;
        } else {
            const u64 addr = 0x10000 + (rng() % 9) * 2048 + (rng() % 32) * 64;
            const u64 size = 1 + rng() % (2048 - (addr & 2047));
            legacy.Collect(addr, size);
            plain.Collect(addr, size);
            diagnostic.CollectT<true, TestHooks>(addr, size);
        }
    }
    CHECK(same);
}

TEST_CASE("t4.0 [5]: collect and Gather interleaved in threads: coverage is kept, lot claims are valid",
          "[list-diag][t40][stress]") {
    // Coverage: ONE writer per manager (precondition pi4: the production has one writer per core manager) and
    // one Gatherer. Every block written is delivered at least once after it was written (the union of what came
    // out contains it), and nothing outside what was written comes out. The grouping in time between two runs is
    // not required to be the same. NOTE (preexisting, not changed here): with TWO writers on one manager a Collect
    // that takes the guard path can overwrite the other writer's concurrent CAS, and a block is lost; the old
    // manager does it too (3 of 20 rounds in a check made while writing this test). Production does not do it.
    for (int round = 0; round < 3; ++round) {
        Manager m;
        constexpr unsigned kWrites = 8000;
        std::vector<u64> written;
        std::atomic<bool> done{false};
        std::thread writer([&] {
            std::mt19937 rng{static_cast<unsigned>(round)};
            for (unsigned i = 0; i < kWrites; ++i) {
                const u64 block = 0x1000 + (rng() % 2048) * 64; // several 2 KiB transformations
                m.Collect(block, 64);
                written.push_back(block);
                if (rng() % 16 == 0) {
                    std::this_thread::yield();
                }
            }
            done.store(true);
        });
        std::set<u64> delivered_blocks;
        const auto gather = [&] {
            for (const auto& [a, s] : GatherOf(m)) {
                for (u64 b = a; b < a + s; b += 64) {
                    delivered_blocks.insert(b);
                }
            }
        };
        while (!done.load()) {
            gather();
            std::this_thread::yield();
        }
        writer.join();
        gather();
        const std::set<u64> expected(written.begin(), written.end());
        CHECK(delivered_blocks == expected); // all of it, and nothing else
    }
    // The lot claims of the diagnostic stay valid with the manager of T4.0 (ground truth: the Gather itself).
    const unsigned notices = StressNotices();
    for (unsigned round = 1; round <= 2; ++round) {
        const StressResult roomy = RunStress({notices, 4096, 1'000'000'000ull, round + 30, 0});
        CHECK(roomy.bad_claims == 0);
        CHECK(roomy.missing == 0);
        CHECK(roomy.false_windows == 0);
        CHECK(roomy.conserved);
        CHECK(roomy.overlapped == 0);
        CHECK(roomy.false_complete == 0);
    }
}

// T4.0, the ABA, deterministic (revision 2 of the T4 plan). The writer reads the EMPTY `current`; a Gather takes
// and puts the empty back (current holds the same value again); the writer's CAS then succeeds, publishing after
// that Gather. Stress and serial equivalence do not guarantee that this interleaving happened; the stop point
// "c.before_cas" does. No write is lost (the next Gather delivers it) and, with the diagnostic, the notice is
// ambiguous (the Gather changed lot_started between the reads s and s2).
TEST_CASE("t4.0 [ABA]: the empty read, a Gather puts the empty back, the CAS publishes", "[list-diag][t40]") {
    SECTION("the manager alone: nothing is lost") {
        Fx f(16);
        f.gates.Arm("c.before_cas");
        std::thread writer([&] { f.managers[0].CollectT<false, TestHooks>(0x50000, 64); });
        f.gates.WaitReached("c.before_cas");
        Out between;
        std::function<void(PAddr, size_t)> cb = [&](PAddr a, size_t s) { between.emplace_back(a, s); };
        f.managers[0].Gather(cb); // the empty goes out and the empty comes back
        CHECK(between.empty());
        f.gates.Release("c.before_cas");
        writer.join(); // the CAS found the same empty and published
        const Out later = GatherOf(f.managers[0]);
        REQUIRE(later.size() == 1);
        CHECK(later[0] == std::pair<u64, u64>{0x50000, 64});
        CHECK(GatherOf(f.managers[0]).empty());
    }
    SECTION("with the diagnostic the notice is ambiguous and the block comes in the next lot") {
        Fx f(16);
        f.gates.Arm("c.before_cas");
        Proto::Claim claim;
        std::thread writer([&] { claim = f.Collect(0, 0x50000); });
        f.gates.WaitReached("c.before_cas");
        const Proto::Batch first = f.GatherAll(); // lot 1: nothing to deliver
        CHECK(first.pre.empty());
        f.gates.Release("c.before_cas");
        writer.join();
        CHECK(claim.ambiguous); // s2 != s: the Gather started in between
        CHECK(claim.registered);
        const Proto::Batch second = f.GatherAll(); // lot 2: the write comes out here
        REQUIRE(second.pre.size() == 1);
        CHECK(second.pre[0].addr == 0x50000);
        CHECK(second.pre[0].size == 64);
    }
}
