// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Isolated harness of the notice ledger, the lot link and the late-publication check of
// docs/list-diag-design.md (3.3, 3.4). Nothing of the emulator runs here; the only code taken
// from src/ is the real core/gpu_dirty_memory_manager.h, for the equivalence test.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/gpu_dirty_memory_manager.h"
#include "system.h"

using namespace diag;

namespace {

int g_failures = 0;
int g_checks = 0;

void CheckImpl(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL line %d: %s\n", line, expr);
    }
}
void CheckEqImpl(u64 a, u64 b, const char* ea, const char* eb, int line) {
    ++g_checks;
    if (a != b) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL line %d: %s == %s (%llu vs %llu)\n", line, ea, eb,
                     static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    }
}
#define CHECK(c) CheckImpl((c), #c, __LINE__)
#define CHECK_EQ(a, b) CheckEqImpl(static_cast<u64>(a), static_cast<u64>(b), #a, #b, __LINE__)
#define C(f, field) Get((f).sys.counters.field)

struct Fx {
    explicit Fx(unsigned slots = 64) : sys(slots) {
        sys.SetGates(&gates);
        sys.read_guest = [](u64, u8* out, size_t len) { std::memset(out, 0xEE, len); };
    }
    System<3> sys;
    Gates gates;
};

constexpr u8 kBytes[64] = {1, 2, 3, 4};

// ---------------------------------------------------------------------------------------
// T1: Manager<false> is the real manager; Manager<true> delivers the same notices.
void TestEquivalence() {
    std::puts("equivalence: real GPUDirtyMemoryManager == Manager<false> == Manager<true>");
    Core::GPUDirtyMemoryManager real;
    Manager<false> plain;
    Manager<true> diagnostic;
    std::mt19937 rng{1234};
    using Out = std::vector<std::pair<u64, u64>>;
    for (int step = 0; step < 40000; ++step) {
        if (rng() % 7 == 0) {
            Out a, b, c;
            std::function<void(PAddr, size_t)> ca = [&](PAddr x, size_t y) { a.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cb = [&](PAddr x, size_t y) { b.emplace_back(x, y); };
            std::function<void(PAddr, size_t)> cc = [&](PAddr x, size_t y) { c.emplace_back(x, y); };
            real.Gather(ca);
            plain.Gather(cb);
            diagnostic.Gather(cc);
            CHECK(a == b);
            CHECK(a == c);
        } else {
            const u64 page = rng() % 6;
            const u64 half = rng() % 2;
            const u64 off = (rng() % 480) * 4; // keep [off, off + size) inside one 2 KiB region
            const u64 size = 1 + rng() % std::min<u64>(96, 2048 - off);
            const u64 addr = 0x10000 + page * 4096 + half * 2048 + off;
            real.Collect(addr, size);
            plain.Collect(addr, size);
            diagnostic.Collect(addr, size);
        }
    }
}

// ---------------------------------------------------------------------------------------
// T2 and item 3: claims against the identity of each notice, with blocks that repeat.
struct Step {
    bool gather;       // true: a Gather+apply; false: a notice
    u64 addr;
    u64 expected_lot;  // scripted by construction: how many Gathers happened before it, plus 1
};

struct ClaimsResult {
    unsigned identity_failures = 0;
    unsigned address_only_passes = 0;
    unsigned notices = 0;
};

ClaimsResult RunScript(const std::vector<Step>& steps, int mutant_delta) {
    Fx f;
    f.sys.mutant_delta = mutant_delta;
    std::map<u64, std::set<u64>> output_blocks; // lot -> blocks delivered by that Gather (core 0)
    std::vector<std::pair<Claim, u64>> claims;
    for (const Step& s : steps) {
        if (s.gather) {
            const Batch b = f.sys.GatherAll();
            for (const Range& r : b.pre) {
                if (r.mask != 1) continue;
                for (u64 a = r.addr; a < r.addr + r.size; a += 64) {
                    output_blocks[b.lots[0]].insert(a);
                }
            }
            f.sys.ApplyBatchRanges(b);
        } else {
            claims.emplace_back(f.sys.CollectObserved(0, s.addr, 4), s.expected_lot);
            claims.back().second = s.expected_lot;
        }
    }
    ClaimsResult res;
    size_t index = 0;
    for (const Step& s : steps) {
        if (s.gather) continue;
        const Claim& c = claims[index++].first;
        ++res.notices;
        if (c.ambiguous || c.lot != s.expected_lot) {
            ++res.identity_failures;
        }
        const u64 block = s.addr & ~u64{63};
        // The check that is not enough: "the claimed lot delivered this address".
        if (!c.ambiguous && output_blocks[c.lot].count(block) != 0) {
            ++res.address_only_passes;
        }
    }
    return res;
}

void TestClaims() {
    std::puts("claims: the lot of each notice, with the same 64-byte block in several lots");
    // The same block in lots 1, 2, 3 and twice in lot 4 (the second one is "already covered").
    const std::vector<Step> repeated{
        {false, 0x2000, 1}, {true, 0, 0},   {false, 0x2008, 2}, {true, 0, 0},  {false, 0x2000, 3},
        {true, 0, 0},       {false, 0x2004, 4}, {false, 0x2004, 4}, {true, 0, 0},
    };
    const ClaimsResult ok = RunScript(repeated, 0);
    CHECK_EQ(ok.notices, 5);
    CHECK_EQ(ok.identity_failures, 0);
    CHECK_EQ(ok.address_only_passes, 5);
    // A mutant that claims the next lot: the identity check sees every wrong claim; the
    // address-only check is fooled whenever the block shows up in the wrong lot too.
    const ClaimsResult mutant = RunScript(repeated, 1);
    CHECK_EQ(mutant.identity_failures, 5);
    CHECK_EQ(mutant.address_only_passes, 3);
    std::printf("  mutant: identity check fails %u of %u, address-only check is fooled %u times\n",
                mutant.identity_failures, mutant.notices, mutant.address_only_passes);

    // The three paths of Collect (core/gpu_dirty_memory_manager.h:26-47), all in lot 1:
    // CAS into an empty `current`, the guard path (another 2 KiB region), "already covered",
    // and the guard path again.
    const std::vector<Step> paths{
        {false, 0x3000, 1}, {false, 0x4000, 1}, {false, 0x4000, 1}, {false, 0x3000, 1}, {true, 0, 0},
    };
    const ClaimsResult p = RunScript(paths, 0);
    CHECK_EQ(p.notices, 4);
    CHECK_EQ(p.identity_failures, 0);
    CHECK_EQ(p.address_only_passes, 4);
}

// ---------------------------------------------------------------------------------------
// Ambiguity: a Gather running, or starting, around the registration.
void TestAmbiguous() {
    std::puts("ambiguous: a retirement concurrent with the registration is never guessed");
    { // (a) a Gather incremented lot_started and has not taken the guard yet
        Fx f;
        f.gates.Arm("g.after_inc");
        Batch b;
        std::thread gt([&] { b = f.sys.GatherAll(); });
        f.gates.WaitReached("g.after_inc");
        const Claim c = f.sys.CollectObserved(0, 0x6000, 4);
        CHECK(c.ambiguous);
        f.gates.Release("g.after_inc");
        gt.join();
        f.sys.ApplyBatchRanges(b);
        CHECK_EQ(C(f, apply_overlap_ambiguous_lot), 1);
        CHECK_EQ(C(f, notice_applied), 0);
    }
    { // (b) the Gather takes the notice between the Collect and the second read
        Fx f;
        f.gates.Arm("w.after_collect");
        Claim c;
        std::thread wt([&] { c = f.sys.CollectObserved(0, 0x6100, 4); });
        f.gates.WaitReached("w.after_collect");
        f.sys.InvalidateDiag();
        f.gates.Release("w.after_collect");
        wt.join();
        CHECK(c.ambiguous);
    }
    { // (c) a whole Gather between the first reads and the Collect
        Fx f;
        f.gates.Arm("w.after_r0");
        Claim c;
        std::thread wt([&] { c = f.sys.CollectObserved(0, 0x6200, 4); });
        f.gates.WaitReached("w.after_r0");
        f.sys.InvalidateDiag();
        f.gates.Release("w.after_r0");
        wt.join();
        CHECK(c.ambiguous);
    }
    { // (d) lot_done already written, the callbacks not run yet: the notice is of the next lot
        Fx f;
        f.gates.Arm("g.before_iter");
        Batch b;
        std::thread gt([&] { b = f.sys.GatherAll(); });
        f.gates.WaitReached("g.before_iter");
        const Claim c = f.sys.CollectObserved(0, 0x6300, 4);
        CHECK(!c.ambiguous);
        CHECK_EQ(c.lot, 2);
        f.gates.Release("g.before_iter");
        gt.join();
        f.sys.ApplyBatchRanges(b);
        f.sys.InvalidateDiag(); // lot 2 delivers the block and retires the record
        CHECK_EQ(C(f, notice_applied), 1);
    }
}

// ---------------------------------------------------------------------------------------
// The counterexample of the review: A is taken, B is registered in the same block, the
// application of A must not retire B.
void TestLotCounterexample() {
    std::puts("lot counterexample: the application of A finds B in the same block");
    Fx f;
    const Claim a = f.sys.CollectObserved(0, 0x7000, 4);
    CHECK_EQ(a.lot, 1);
    const Batch b1 = f.sys.GatherAll(); // takes A (lot 1); not applied yet
    const Claim b = f.sys.CollectObserved(0, 0x7008, 4);
    CHECK(!b.ambiguous);
    CHECK_EQ(b.lot, 2); // taken after lot 1 completed: the next lot
    f.sys.ApplyBatchRanges(b1);
    CHECK_EQ(C(f, notice_applied), 1);          // A, and only A
    CHECK_EQ(C(f, apply_overlap_other_lot), 1); // B overlaps, other lot: counted, not retired
    f.sys.Pop(0x7008, 0x700c, kBytes);          // B is still pending: the pop counts
    CHECK_EQ(C(f, pop_over_notice), 1);
    f.sys.InvalidateDiag(); // lot 2: B retired now
    CHECK_EQ(C(f, notice_applied), 2);
    CHECK_EQ(f.sys.PublishedSlots(), 0);
}

// A notice taken by the Gather and not yet applied is still pending, though the manager no
// longer holds it (the reason there is no peek at the manager).
void TestBetweenGatherAndApply() {
    std::puts("between the Gather and the application: the manager is empty, the ledger is not");
    Fx f;
    f.sys.CollectObserved(0, 0xD000, 4);
    const Batch b = f.sys.GatherAll();
    f.sys.Pop(0xD000, 0xD004, kBytes);
    CHECK_EQ(C(f, pop_over_notice), 1);
    const Batch again = f.sys.GatherAll();
    CHECK(again.pre.empty());
    CHECK(Get(f.sys.gather_empty[0]) >= 1);
    f.sys.ApplyBatchRanges(b);
    CHECK_EQ(C(f, notice_applied), 1);
}

// ---------------------------------------------------------------------------------------
// Item 2 of the review: a notice published after the application of its lot.
void TestLatePublication() {
    std::puts("late publication: a notice published after the application of its lot");
    { // (a) paused between the second read and the publication; the whole application runs
        Fx f;
        f.gates.Arm("w.after_s2");
        Claim c;
        std::thread wt([&] { c = f.sys.CollectObserved(0, 0x8000, 4); });
        f.gates.WaitReached("w.after_s2");
        f.sys.InvalidateDiag(); // takes lot 1 (the Collect is done), stores 1, sweeps: nothing
        f.gates.Release("w.after_s2");
        wt.join();
        CHECK(c.registered && !c.ambiguous);
        CHECK_EQ(c.lot, 1);
        CHECK_EQ(C(f, notice_late), 1);
        CHECK_EQ(f.sys.PublishedSlots(), 0);
        f.sys.Pop(0x8000, 0x8004, kBytes);
        CHECK_EQ(C(f, pop_over_notice), 0); // no false window
        CHECK_EQ(C(f, pop_over_unchecked), 0);
    }
    { // the same, without the check: the test is sensitive (the false window appears)
        Fx f;
        f.sys.naive_no_late_check = true;
        f.gates.Arm("w.after_s2");
        std::thread wt([&] { f.sys.CollectObserved(0, 0x8000, 4); });
        f.gates.WaitReached("w.after_s2");
        f.sys.InvalidateDiag();
        f.gates.Release("w.after_s2");
        wt.join();
        f.sys.Pop(0x8000, 0x8004, kBytes);
        CHECK_EQ(C(f, pop_over_notice), 1); // the false window of the review's counterexample
    }
    { // (b) paused between the publication and the read of apply_started_lot
        Fx f;
        f.gates.Arm("w.after_publish");
        std::thread wt([&] { f.sys.CollectObserved(0, 0x8100, 4); });
        f.gates.WaitReached("w.after_publish");
        f.sys.Pop(0x8100, 0x8104, kBytes); // published, not checked: no claim
        CHECK_EQ(C(f, pop_over_unchecked), 1);
        CHECK_EQ(C(f, pop_over_notice), 0);
        f.sys.InvalidateDiag(); // the sweep sees the published record and retires it
        CHECK_EQ(C(f, notice_applied), 1);
        f.gates.Release("w.after_publish");
        wt.join();
        CHECK_EQ(C(f, notice_late), 0); // already retired: exactly one end state
        CHECK_EQ(C(f, notice_applied), 1);
        CHECK_EQ(f.sys.PublishedSlots(), 0);
    }
    { // (c) the application of lot 1 is running (lot stored, sweep not started) when it publishes
        Fx f;
        f.gates.Arm("w.after_s2");
        f.gates.Arm("a.after_astore");
        Claim c;
        std::thread wt([&] { c = f.sys.CollectObserved(0, 0x8200, 4); });
        f.gates.WaitReached("w.after_s2");
        const Batch b = f.sys.GatherAll();
        std::thread at([&] { f.sys.ApplyBatchRanges(b); });
        f.gates.WaitReached("a.after_astore");
        f.gates.Release("w.after_s2"); // publishes, reads apply_started_lot == 1: late
        wt.join();
        f.gates.Release("a.after_astore"); // the sweep finds nothing
        at.join();
        CHECK_EQ(C(f, notice_late), 1);
        CHECK_EQ(C(f, notice_applied), 0);
        CHECK_EQ(f.sys.PublishedSlots(), 0);
    }
    { // (d) published and checked before the application starts: a real window, then retired
        Fx f;
        f.sys.CollectObserved(0, 0x8300, 4);
        f.sys.Pop(0x8300, 0x8304, kBytes);
        CHECK_EQ(C(f, pop_over_notice), 1);
        f.sys.InvalidateDiag();
        CHECK_EQ(C(f, notice_applied), 1);
        CHECK_EQ(C(f, notice_late), 0);
    }
}

// ---------------------------------------------------------------------------------------
// Item 3 of the first review: ownership and generation of the slots.
void TestRecycling() {
    std::puts("recycling: the slot is reused while a reader reads, marks the pop, marks the apply");
    { // (i) while the payload is read
        Fx f(1);
        f.sys.ledger.expiry_us = 100;
        f.sys.CollectObserved(0, 0x9000, 4);
        f.gates.Arm("r.after_payload_read");
        std::thread pt([&] { f.sys.Pop(0x9000, 0x9004, kBytes); });
        f.gates.WaitReached("r.after_payload_read");
        f.sys.clock += 200;
        f.sys.CollectObserved(0, 0x9100, 4); // reclaims the expired slot
        CHECK_EQ(C(f, notice_expired), 1);
        f.gates.Release("r.after_payload_read");
        pt.join();
        CHECK(C(f, observation_lost) >= 1);
        CHECK_EQ(C(f, pop_over_notice), 0);
        f.sys.Pop(0x9100, 0x9104, kBytes); // the new record is intact
        CHECK_EQ(C(f, pop_over_notice), 1);
    }
    { // (ii) while the pop marks t_pop
        Fx f(1);
        f.sys.ledger.expiry_us = 100;
        const Claim c1 = f.sys.CollectObserved(0, 0x9200, 4);
        f.gates.Arm("m.before_pop_cas");
        std::thread pt([&] { f.sys.Pop(0x9200, 0x9204, kBytes); });
        f.gates.WaitReached("m.before_pop_cas");
        f.sys.clock += 200;
        const Claim c2 = f.sys.CollectObserved(0, 0x9300, 4);
        CHECK(c2.registered);
        f.gates.Release("m.before_pop_cas");
        pt.join();
        CHECK_EQ(C(f, observation_lost), 1);
        CHECK_EQ(C(f, pop_over_notice), 0);
        CHECK((f.sys.ledger.Marks(*c2.handle.slot) & kBitPopped) == 0); // never marks a new generation
        (void)c1;
    }
    { // (iii) while the application marks t_apply
        Fx f(1);
        f.sys.ledger.expiry_us = 100;
        f.sys.CollectObserved(0, 0x9400, 4);
        const Batch b = f.sys.GatherAll();
        f.gates.Arm("m.before_apply_cas");
        std::thread at([&] { f.sys.ApplyBatchRanges(b); });
        f.gates.WaitReached("m.before_apply_cas");
        f.sys.clock += 200;
        const Claim c2 = f.sys.CollectObserved(0, 0x9500, 4);
        CHECK(c2.registered);
        f.gates.Release("m.before_apply_cas");
        at.join();
        CHECK_EQ(C(f, observation_lost), 1);
        CHECK_EQ(C(f, notice_applied), 0);
        CHECK((f.sys.ledger.Marks(*c2.handle.slot) & kBitApplied) == 0); // the new record is pending
        f.sys.Pop(0x9500, 0x9504, kBytes);
        CHECK_EQ(C(f, pop_over_notice), 1);
    }
    { // (iv-a) a payload of another generation is never taken for the current one
        Fx f;
        const Claim c = f.sys.CollectObserved(0, 0x9600, 4);
        f.sys.Pop(0x9600, 0x9604, kBytes);
        f.sys.ledger.TestWritePopWords(*c.handle.slot, (c.handle.gen16 + 7) & 0xffff, 0x9600, 4, 1);
        f.sys.InvalidateDiag();
        CHECK_EQ(C(f, window_unverified), 1);
        CHECK_EQ(C(f, window_sample_equal) + C(f, window_sample_different), 0);
    }
    { // (iv-b) a pop payload written after the slot was reused: lost, then overwritten
        Fx f(1);
        f.sys.ledger.expiry_us = 100;
        f.sys.read_guest = [](u64, u8* out, size_t len) { std::memcpy(out, kBytes, len); };
        f.sys.CollectObserved(0, 0x9700, 4);
        f.gates.Arm("m.before_pop_payload");
        std::thread pt([&] { f.sys.Pop(0x9700, 0x9704, kBytes); });
        f.gates.WaitReached("m.before_pop_payload");
        f.sys.clock += 200;
        f.sys.CollectObserved(0, 0x9800, 4);
        f.gates.Release("m.before_pop_payload"); // writes the old generation's words, then loses the CAS
        pt.join();
        CHECK_EQ(C(f, observation_lost), 1);
        f.sys.Pop(0x9800, 0x9804, kBytes); // a fresh pop overwrites them
        CHECK_EQ(C(f, pop_over_notice), 1);
        f.sys.InvalidateDiag();
        CHECK_EQ(C(f, window_sample_equal), 1);
        CHECK_EQ(C(f, window_unverified), 0);
    }
}

// ---------------------------------------------------------------------------------------
// Item 1 of the review: the precondition (one Gather at a time per manager), and what the
// diagnostic does when it does not hold.
void TestOverlap() {
    std::puts("overlap: two Gathers on one manager are counted, and the lots up to there are suspect");
    Fx f;
    f.sys.managers[0].skip_gather_on_overlap = true; // the race itself is not executed
    const Claim c = f.sys.CollectObserved(0, 0xC000, 4);
    CHECK_EQ(c.lot, 1);
    f.gates.Arm("g.before_iter");
    Batch b;
    std::thread at([&] { b = f.sys.GatherAll(); });
    f.gates.WaitReached("g.before_iter");
    u64 lot = 99;
    std::function<void(PAddr, size_t)> cb = [](PAddr, size_t) {};
    f.sys.managers[0].Gather(cb, &lot); // a second Gather while the first one iterates
    CHECK_EQ(lot, 0);
    CHECK_EQ(f.sys.managers[0].gather_overlapped.load(), 1);
    f.gates.Release("g.before_iter");
    at.join();
    f.sys.Pop(0xC000, 0xC004, kBytes);
    CHECK_EQ(C(f, pop_over_ambiguous), 1); // lot 1 <= floor: no window claimed
    CHECK_EQ(C(f, pop_over_notice), 0);
}

// An application out of order (a lot not above the last one) is a violation as well.
void TestApplyOutOfOrder() {
    std::puts("apply out of order: counted, the lots up to started become suspect");
    Fx f;
    f.sys.CollectObserved(0, 0xC100, 4);
    const Batch b1 = f.sys.GatherAll();
    const Batch b2 = f.sys.GatherAll();
    f.sys.ApplyBatchRanges(b2); // lot 2 first
    f.sys.ApplyBatchRanges(b1); // lot 1 after lot 2
    CHECK_EQ(C(f, apply_out_of_order), 3); // one per manager: all three were applied out of order
}

// ---------------------------------------------------------------------------------------
// The join of the ranges across cores: the set of (core, lot) of the final range.
void TestMergedSets() {
    std::puts("merged ranges: the application receives the set of (core, lot)");
    Fx f;
    f.sys.CollectObserved(0, 0x5000, 4);
    f.sys.CollectObserved(1, 0x5040, 4);
    const Batch b = f.sys.GatherAll();
    CHECK_EQ(b.merged.size(), 1);
    CHECK_EQ(b.merged[0].mask, 3u);
    const Claim c = f.sys.CollectObserved(2, 0x5020, 4); // core 2, inside the joined range
    CHECK_EQ(c.lot, 2);
    f.sys.ApplyBatchRanges(b);
    CHECK_EQ(C(f, notice_applied), 2);          // cores 0 and 1, through one range
    CHECK_EQ(C(f, apply_overlap_other_lot), 1); // core 2's notice is of lot 2
}

// Limitation of docs/list-diag-design.md 3.5: the label does not tell a pop that overwrote the
// write from an application that read before the memcpy.
void TestSampleLabels() {
    std::puts("sample labels: equal is consistent with an overwrite and with a memcpy not yet done");
    Fx f;
    std::map<u64, std::array<u8, 4>> guest;
    f.sys.read_guest = [&](u64 addr, u8* out, size_t len) { std::memcpy(out, guest[addr].data(), len); };
    f.sys.CollectObserved(0, 0xE000, 4);
    f.sys.CollectObserved(0, 0xE100, 4);
    guest[0xE000] = {1, 2, 3, 4}; // the memcpy has not landed: the guest still holds the pop's bytes
    guest[0xE100] = {9, 9, 9, 9}; // newer bytes are there
    f.sys.Pop(0xE000, 0xE004, kBytes);
    f.sys.Pop(0xE100, 0xE104, kBytes);
    f.sys.InvalidateDiag();
    CHECK_EQ(C(f, window_sample_equal), 1);
    CHECK_EQ(C(f, window_sample_different), 1);
    for (const Event& e : f.sys.Events()) {
        CHECK_EQ(e.sample_bytes, 4);
    }
}

// ---------------------------------------------------------------------------------------
// Threads: writers, one invalidator at a time (the precondition), a pop thread.
struct StressConfig {
    unsigned notices = 4000;
    unsigned slots = 64;
    u64 expiry = 1'000'000;
    unsigned seed = 1;
    int mutant = 0; // claim the wrong lot: the ground truth must notice
};

struct Recorded {
    u64 addr;
    Claim claim;
};

void RunStress(const StressConfig& cfg, const char* name) {
    constexpr unsigned kCores = 3;
    auto sys = std::make_unique<System<kCores>>(cfg.slots);
    sys->ledger.expiry_us = cfg.expiry;
    sys->mutant_delta = cfg.mutant;
    sys->read_guest = [](u64, u8* out, size_t len) { std::memset(out, 0xEE, len); };

    std::mutex truth_mutex; // the cache mutex already orders on_*; this one guards first_lot
    std::unordered_set<u64> applied_blocks; // under cache_mutex
    std::atomic<u64> false_windows{0};
    sys->on_range_applied = [&](u64 addr, u64 size) {
        for (u64 a = addr & ~u64{63}; a < addr + size; a += 64) {
            applied_blocks.insert(a);
        }
    };
    sys->on_window_flag = [&](u64 addr, u32) {
        if (applied_blocks.count(addr & ~u64{63}) != 0) {
            false_windows.fetch_add(1);
        }
    };

    std::vector<std::vector<Recorded>> recorded(kCores);
    std::array<std::unordered_map<u64, u64>, kCores> first_lot; // block -> first lot delivering it
    std::mutex invalidate_mutex; // the precondition: one Gather at a time
    std::atomic<unsigned> writers_done{0};
    std::array<std::atomic<u64>, 64> recent{};
    std::atomic<u64> recent_index{0};

    auto invalidate = [&] {
        std::lock_guard lock{invalidate_mutex};
        const Batch b = sys->GatherAll();
        for (const Range& r : b.pre) {
            for (unsigned core = 0; core < kCores; ++core) {
                if ((r.mask >> core) & 1) {
                    for (u64 a = r.addr; a < r.addr + r.size; a += 64) {
                        auto it = first_lot[core].find(a);
                        if (it == first_lot[core].end()) {
                            first_lot[core][a] = b.lots[core];
                        }
                    }
                }
            }
        }
        sys->ApplyBatchRanges(b);
    };

    std::vector<std::thread> threads;
    for (unsigned core = 0; core < kCores; ++core) {
        threads.emplace_back([&, core] {
            std::mt19937 rng{cfg.seed * 100 + core};
            for (unsigned i = 0; i < cfg.notices; ++i) {
                const u64 id = core * 1'000'000ull + i;
                const u64 addr = 0x100000 + id * 64 + (i % 16) * 4;
                const Claim c = sys->CollectObserved(core, addr, 4);
                recorded[core].push_back({addr, c});
                recent[recent_index.fetch_add(1) % recent.size()].store(addr);
                sys->clock.fetch_add(10);
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
                sys->Pop(block, block + 64, bytes.data());
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

    // Checks. I1: an unambiguous claim is the first lot that delivered the block.
    u64 unambiguous = 0, ambiguous = 0, registered = 0, bad_claims = 0, missing = 0;
    for (unsigned core = 0; core < kCores; ++core) {
        for (const Recorded& r : recorded[core]) {
            if (!r.claim.registered) continue;
            ++registered;
            if (r.claim.ambiguous) {
                ++ambiguous;
                continue;
            }
            ++unambiguous;
            const auto it = first_lot[core].find(r.addr & ~u64{63});
            if (it == first_lot[core].end()) {
                ++missing;
            } else if (it->second != r.claim.lot) {
                ++bad_claims;
            }
        }
    }
    if (cfg.mutant != 0) {
        // The ground truth is not vacuous: a wrong lot is seen (the blocks are unique here, so
        // the address-only check would see it too; the scripted tests cover repeated blocks).
        CHECK(bad_claims + missing > 0);
        std::printf("  %s (mutant): %llu wrong claims seen of %llu unambiguous\n", name,
                    static_cast<unsigned long long>(bad_claims + missing),
                    static_cast<unsigned long long>(unambiguous));
        return;
    }
    CHECK_EQ(bad_claims, 0);
    CHECK_EQ(missing, 0);
    // I2: no pop flagged a record whose block had been applied.
    CHECK_EQ(false_windows.load(), 0);
    // I3: every notice has one end state.
    const u64 total = static_cast<u64>(cfg.notices) * kCores;
    CHECK_EQ(registered + Get(sys->counters.notice_overflow), total);
    CHECK_EQ(registered, Get(sys->counters.notice_registered));
    CHECK_EQ(Get(sys->counters.notice_registered),
             Get(sys->counters.notice_applied) + Get(sys->counters.notice_late) +
                 Get(sys->counters.notice_expired) + sys->PublishedSlots());
    u64 overlapped = 0;
    for (auto& m : sys->managers) {
        overlapped += m.gather_overlapped.load();
    }
    CHECK_EQ(overlapped, 0); // the harness respects the precondition
    CHECK_EQ(Get(sys->counters.apply_out_of_order), 0);
    std::printf("  %s: notices=%llu unambiguous=%llu ambiguous=%llu applied=%llu late=%llu expired=%llu "
                "overflow=%llu remaining=%zu | pops over notice=%llu unchecked=%llu ambiguous=%llu | "
                "lost=%llu other_lot=%llu\n",
                name, static_cast<unsigned long long>(total), static_cast<unsigned long long>(unambiguous),
                static_cast<unsigned long long>(ambiguous),
                static_cast<unsigned long long>(Get(sys->counters.notice_applied)),
                static_cast<unsigned long long>(Get(sys->counters.notice_late)),
                static_cast<unsigned long long>(Get(sys->counters.notice_expired)),
                static_cast<unsigned long long>(Get(sys->counters.notice_overflow)), sys->PublishedSlots(),
                static_cast<unsigned long long>(Get(sys->counters.pop_over_notice)),
                static_cast<unsigned long long>(Get(sys->counters.pop_over_unchecked)),
                static_cast<unsigned long long>(Get(sys->counters.pop_over_ambiguous)),
                static_cast<unsigned long long>(Get(sys->counters.observation_lost)),
                static_cast<unsigned long long>(Get(sys->counters.apply_overlap_other_lot)));
}

void TestStress(unsigned notices, unsigned rounds) {
    std::puts("stress: writers, one invalidator at a time, a pop thread; ground truth from the Gather");
    for (unsigned round = 1; round <= rounds; ++round) {
        RunStress({notices, 4096, 1'000'000'000ull, round}, "roomy");
        RunStress({notices, 8, 400, round + 100}, "tight ");
    }
    RunStress({notices, 4096, 1'000'000'000ull, 99, 1}, "roomy");
}

} // namespace

int main(int argc, char** argv) {
    unsigned notices = 3000;
    unsigned rounds = 3;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--notices") == 0) notices = static_cast<unsigned>(std::atoi(argv[i + 1]));
        if (std::strcmp(argv[i], "--rounds") == 0) rounds = static_cast<unsigned>(std::atoi(argv[i + 1]));
    }
    TestEquivalence();
    TestClaims();
    TestAmbiguous();
    TestLotCounterexample();
    TestBetweenGatherAndApply();
    TestLatePublication();
    TestRecycling();
    TestOverlap();
    TestApplyOutOfOrder();
    TestMergedSets();
    TestSampleLabels();
    TestStress(notices, rounds);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::puts("SOME EXPECTATIONS FAILED");
        return 1;
    }
    std::puts("ALL EXPECTATIONS MET");
    return 0;
}
