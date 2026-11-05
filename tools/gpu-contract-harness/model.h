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

// Mechanisms the model implements correctly by default. Turning one off is how the
// negative cases prove that the verifier catches that mechanism failing.
struct ModelKnobs {
    bool hold_cpu_writes = true;            // hold uploads of CPU writes over unfetched entries
    bool page_granular_upload = false;      // upload whole pages instead of the written words
    bool honor_valid_until = true;          // consumer checks Transfer::valid_until
    bool acquire_uses_guest_value = false;  // syncpoint acquire on IncrementGuest, not host
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

enum class Expect { Pass, FailInSomeForcedSchedule, Info };

struct Case {
    std::string name;
    std::string title;
    std::vector<std::vector<Op>> channels;
    std::map<std::uint32_t, Entry> entries;
    ModelKnobs knobs{};
    Expect after_satisfied = Expect::Pass;
    Expect exact_drain = Expect::Pass;
    std::vector<std::uint32_t> expect_suspended;   // channels left suspended
    std::vector<std::uint32_t> expect_continued;   // channels that continued after timeout
    std::vector<std::uint32_t> expect_not_fetched; // entries never fetched
};

struct RunResult {
    bool ok = true;
    std::string failure;
    std::uint64_t missed_writes = 0;
    std::string missed_diagnostic;
    bool suspended = false;
    std::string suspend_diagnostic;
    bool early_completion_observed = false;
};

RunResult RunCase(const Case& c, const contracts::VisibilityRule& rule, Schedule schedule,
                  std::uint64_t seed);

} // namespace harness
