// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Fake front, back and GPU threads around the contracts in contracts.h.
//
// Real threads: the front (the caller of RunCase) runs the channel programs and the
// list fetch consumer; the back thread records ring items (marks the page filter,
// uploads CPU writes, records copies); the "GPU" thread executes recorded commands with
// its own independent random delay and is the only one touching the host buffer.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "contracts.h"

namespace harness {

using contracts::Seq;
using contracts::Word;

constexpr std::uint32_t kPageWords = 1024; // 4 KiB pages of 32-bit words
constexpr std::uint32_t kPages = 16;
constexpr std::uint32_t kWords = kPageWords * kPages;
constexpr std::uint32_t kSyncObjects = 4;

// Initial content of guest memory (and of the host buffer before anything ran).
constexpr Word Original(std::uint32_t pa) {
    return 0xC0000000u | pa;
}
constexpr Word ProducerWord(std::uint32_t producer, std::uint32_t k) {
    return 0xA0000000u | (producer << 16) | k;
}

enum class Schedule {
    Random,   // random delays in all three threads, nothing forced
    BackLags, // back records only what the front explicitly waits for
    GpuLags,  // GPU executes only what the front explicitly waits for
    BothLag,  // both of the above
    Eager,    // after every push the front waits for back and GPU to drain (no window)
};
const char* ScheduleName(Schedule s);

// When the host value of a syncpoint rises. Fast with the default fence behaviour runs the
// SignalFence callback (IncrementHost) when the fence is signalled, that is, when the back
// thread processes the increment (fence_manager.h:123-130). With deferred fences (level High,
// or GPU Fence Behavior Balanced/Accurate) it runs when the GPU completes the fence.
enum class HostMode {
    Early,    // host-early: the back thread raises the host value when it records the increment
    Deferred, // host-deferred: the GPU raises it when it executes the increment
};
const char* HostModeName(HostMode m);

// Mechanisms the model implements correctly by default. Turning one off is how the
// negative cases prove that the verifier catches that mechanism failing.
struct ModelKnobs {
    bool hold_cpu_writes = true;            // hold uploads of CPU writes over unfetched entries
    bool page_granular_upload = false;      // upload whole pages instead of the written words
    bool honor_valid_until = true;          // consumer checks Transfer::valid_until
    bool acquire_uses_guest_value = false;  // syncpoint acquire on IncrementGuest, not host
    bool trust_syncpoint_as_ready = false;  // consumer reads the host buffer after the acquire
    bool cancel_pending_downloads = true;   // a CPU write cancels pending downloads over it
    bool single_pending_set = false;        // one pending set for all downloads (pre 15126fb)
};

enum class OpKind {
    Produce,       // GPU work writing `values` at GPU VA `va`
    Wfi,           // wait for idle: order marker
    SyncBit,       // sync bit of an entry without Sync Memory Operations: order marker
    SyncptIncr,    // IncrementGuest now (early, as in Fast), IncrementHost at GPU completion
    SyncptAcquire, // channel parks until host syncpoint[index] >= value
    SemRelease,    // GPU writes semaphore[index] = value at completion
    SemAcquire,    // channel parks until semaphore[index] >= value
    CpuWrite,      // guest CPU writes `values` at physical address `pa`
    GuestObserve,  // guest CPU checks that it sees syncpoint[index] >= value
    Remap,         // GPU VA page `page_va` now maps physical page `page_pa`
    Submit,        // entry submitted: snapshot of its words through the current mapping
    Fetch,         // list fetch of entry words [offset, offset + len)
    CpuRead,       // guest CPU reads physical [pa, pa + len): downloads copy these ranges back
    Fence,         // fence: queue a GPU -> staging download of the GPU written, CPU read words
    WaitBack,      // front waits until the back thread has recorded everything pushed so far
    WaitDownloads, // front waits until every queued download is applied to guest memory
    CheckGuest,    // guest memory [pa, pa + expected.size()) must equal `expected`
    CheckHost,     // host buffer [pa, ...) must equal `expected` once everything is drained
};

struct Op {
    OpKind kind;
    std::uint32_t id = 0; // producer id, or entry id for Submit and Fetch
    std::uint32_t va = 0;
    std::uint32_t pa = 0;
    std::vector<Word> values;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::uint32_t offset = 0;
    std::uint32_t len = 0;
    std::vector<Word> expected;
    bool require_host_syncpoint = false; // Fetch: host syncpoint[index] >= value must hold
    std::chrono::milliseconds timeout{5000};
    contracts::OnTimeout on_timeout = contracts::OnTimeout::Suspend;
    contracts::Satisfier satisfier{};
    std::uint32_t page_va = 0;
    std::uint32_t page_pa = 0;
};

struct Entry {
    std::uint32_t va;
    std::uint32_t len;
};

// Kind of a failed run. A negative case declares the one kind it must produce; any
// other kind showing up means the case broke for a different reason and fails the test.
enum class FailureKind : std::uint32_t {
    WordMismatch = 1u << 0,       // a fetch saw words other than the expected version
    DependencyViolated = 1u << 1, // an entry ran before its dependency held
    UnexpectedSuspend = 1u << 2,  // a channel did not finish (left suspended or stuck)
    MissingSuspend = 1u << 3,     // a channel that had to end suspended did not
    ContinueMismatch = 1u << 4,   // on_timeout=Continue taken or not, contrary to the case
    Timeout = 1u << 5,            // a dependency hit its limit with on_timeout=Abort
    Hang = 1u << 6,               // watchdog: the run did not finish
    CaseError = 1u << 7,          // the script itself is inconsistent
};
const char* FailureKindName(FailureKind kind);
std::string FailureKindsName(std::uint32_t kinds);

struct Expect {
    enum class Mode { Pass, Fail, Info } mode = Mode::Pass;
    FailureKind kind = FailureKind::WordMismatch; // Fail: the only kind allowed

    static Expect MustPass() {
        return {};
    }
    // Must fail in at least one forced schedule, and only with `k`, in every schedule.
    static Expect MustFail(FailureKind k) {
        return {Mode::Fail, k};
    }
    static Expect Informative() {
        return {Mode::Info, FailureKind::WordMismatch};
    }
};

struct Case {
    std::string name;
    std::string title;
    std::vector<std::vector<Op>> channels;
    std::map<std::uint32_t, Entry> entries;
    ModelKnobs knobs{};
    Expect after_satisfied = Expect::MustPass();
    Expect exact_drain = Expect::MustPass();
    // Overrides for the host-early mode; unset means the same as in host-deferred.
    std::optional<Expect> early_after_satisfied;
    std::optional<Expect> early_exact_drain;
    // host-early only: in the gpu-lags and both-lag schedules the acquire must be satisfied
    // while the GPU has not executed what was recorded (the case exercises "syncpoint first").
    bool require_ahead_early = false;
    Expect ExpectFor(bool after_satisfied_rule, HostMode mode) const {
        if (mode == HostMode::Early) {
            const auto& o = after_satisfied_rule ? early_after_satisfied : early_exact_drain;
            if (o) {
                return *o;
            }
        }
        return after_satisfied_rule ? after_satisfied : exact_drain;
    }
    std::vector<std::uint32_t> expect_suspended;   // channels left suspended
    std::vector<std::uint32_t> expect_continued;   // channels that continued after timeout
    std::vector<std::uint32_t> expect_not_fetched; // entries never fetched
};

struct RunResult {
    bool ok = true;
    std::uint32_t failure_kinds = 0; // FailureKind bits of every failure in the run
    std::string failure;             // message of the first one
    std::uint64_t missed_writes = 0;
    std::string missed_diagnostic;
    bool suspended = false;
    std::string suspend_diagnostic;
    bool early_completion_observed = false;
    bool syncpoint_ahead_of_gpu = false; // an acquire passed while the GPU lagged the recording
};

RunResult RunCase(const Case& c, const contracts::VisibilityRule& rule, Schedule schedule,
                  HostMode host_mode, std::uint64_t seed);

} // namespace harness
