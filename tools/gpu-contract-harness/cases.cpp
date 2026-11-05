// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Directed cases of docs/gpu-produced-data-plan.md 4.1.4 (plus remapping), each with the
// version every fetch must see, written out here independently of the model.
//
// Version of an entry (plan 4.2): the submission snapshot plus the ranges written by every
// producer pushed to the ring before the entry, and only those, later ones winning.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "model.h"

using namespace harness;
using contracts::OnTimeout;
using contracts::Satisfier;
using contracts::SatisfierKind;

namespace {

constexpr std::uint32_t Page(std::uint32_t p) {
    return p * kPageWords;
}

std::vector<Word> ProducerValues(std::uint32_t id, std::uint32_t n) {
    std::vector<Word> v(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        v[k] = ProducerWord(id, k);
    }
    return v;
}

// Expected words of a segment: original memory at `pa`, then overlays.
struct Expected {
    std::vector<Word> words;
    Expected(std::uint32_t pa, std::uint32_t len) : words(len) {
        for (std::uint32_t i = 0; i < len; ++i) {
            words[i] = Original(pa + i);
        }
    }
    Expected& Put(std::uint32_t at, const std::vector<Word>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            words[at + i] = v[i];
        }
        return *this;
    }
};

Op Produce(std::uint32_t id, std::uint32_t va, std::uint32_t n) {
    Op op{OpKind::Produce};
    op.id = id;
    op.va = va;
    op.values = ProducerValues(id, n);
    return op;
}
Op Simple(OpKind k) {
    return Op{k};
}
Op SyncptIncr(std::uint32_t index) {
    Op op{OpKind::SyncptIncr};
    op.index = index;
    return op;
}
Op SyncptAcquire(std::uint32_t index, std::uint32_t value, Satisfier who,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                 OnTimeout on_timeout = OnTimeout::Suspend) {
    Op op{OpKind::SyncptAcquire};
    op.index = index;
    op.value = value;
    op.satisfier = who;
    op.timeout = timeout;
    op.on_timeout = on_timeout;
    return op;
}
Op SemRelease(std::uint32_t index, std::uint32_t value) {
    Op op{OpKind::SemRelease};
    op.index = index;
    op.value = value;
    return op;
}
Op SemAcquire(std::uint32_t index, std::uint32_t value, Satisfier who) {
    Op op{OpKind::SemAcquire};
    op.index = index;
    op.value = value;
    op.satisfier = who;
    return op;
}
Op CpuWrite(std::uint32_t pa, std::vector<Word> values) {
    Op op{OpKind::CpuWrite};
    op.pa = pa;
    op.values = std::move(values);
    return op;
}
Op GuestObserve(std::uint32_t index, std::uint32_t value) {
    Op op{OpKind::GuestObserve};
    op.index = index;
    op.value = value;
    return op;
}
Op Remap(std::uint32_t page_va, std::uint32_t page_pa) {
    Op op{OpKind::Remap};
    op.page_va = page_va;
    op.page_pa = page_pa;
    return op;
}
Op Submit(std::uint32_t entry) {
    Op op{OpKind::Submit};
    op.id = entry;
    return op;
}
Op Fetch(std::uint32_t entry, std::uint32_t offset, const Expected& e) {
    Op op{OpKind::Fetch};
    op.id = entry;
    op.offset = offset;
    op.len = static_cast<std::uint32_t>(e.words.size());
    op.expected = e.words;
    return op;
}
Op FetchAfterSyncpoint(std::uint32_t entry, const Expected& e, std::uint32_t index,
                       std::uint32_t value) {
    Op op = Fetch(entry, 0, e);
    op.require_host_syncpoint = true;
    op.index = index;
    op.value = value;
    return op;
}

const Satisfier kChannel1{SatisfierKind::Channel, 1};

std::vector<Case> BuildCases() {
    std::vector<Case> cases;

    { // First write: the page was never marked; the producer is still in the ring when
      // the front reaches the entry, with a WFI between them.
        Case c{"first_write", "primeira escrita (WFI entre produtor e fetch)"};
        c.entries[1] = {Page(3), 128};
        c.channels = {{Produce(1, Page(3), 64), Simple(OpKind::Wfi), Submit(1),
                       Fetch(1, 0, Expected(Page(3), 128).Put(0, ProducerValues(1, 64)))}};
        cases.push_back(c);
    }
    { // Negative for the hypothesis: nothing between producer and fetch.
        Case c{"no_dependency", "produtor sem dependencia (negativo da regra)"};
        c.entries[1] = {Page(4), 64};
        c.channels = {{Produce(1, Page(4) + 16, 32), Submit(1),
                       Fetch(1, 0, Expected(Page(4), 64).Put(16, ProducerValues(1, 32)))}};
        c.after_satisfied = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // Producer in channel 1, consumer in channel 0 after a syncpoint acquire. Channel 0
      // is scheduled first, so it parks and must not block channel 1.
        Case c{"other_channel", "produtor de outro canal (acquire de syncpoint)"};
        c.entries[1] = {Page(5), 64};
        c.channels = {
            {Submit(1), SyncptAcquire(0, 1, kChannel1),
             FetchAfterSyncpoint(1, Expected(Page(5), 64).Put(0, ProducerValues(1, 32)), 0, 1)},
            {Produce(1, Page(5), 32), SyncptIncr(0)}};
        cases.push_back(c);
    }
    { // Acquire in the middle of the entry: words before it come from the snapshot, words
      // after the resume must include the producer that ran before the release.
        Case c{"resume_inside_entry", "retomada dentro da entrada (acquire de semaforo)"};
        c.entries[1] = {Page(6), 64};
        c.channels = {
            {Submit(1), Fetch(1, 0, Expected(Page(6), 16)), SemAcquire(0, 1, kChannel1),
             Fetch(1, 16, Expected(Page(6) + 16, 48).Put(0, ProducerValues(1, 48)))},
            {Produce(1, Page(6) + 16, 48), SemRelease(0, 1)}};
        cases.push_back(c);
    }
    std::vector<Word> garbage(64, 0xDEADBEEFu);
    { // Fast signals completion early; the game sees it and reuses the submitted memory
      // before the emulator fetched the entry. The entry still needs submission + producer.
        Case c{"cpu_reuse", "reuso pela CPU depois de conclusao antecipada"};
        c.entries[1] = {Page(7), 64};
        c.channels = {{Submit(1), Produce(1, Page(7), 32), SyncptIncr(1), Simple(OpKind::Wfi),
                       GuestObserve(1, 1), CpuWrite(Page(7), garbage),
                       Fetch(1, 0, Expected(Page(7), 64).Put(0, ProducerValues(1, 32)))}};
        cases.push_back(c);
        c.name = "cpu_reuse_no_hold";
        c.title = "reuso pela CPU sem segurar escritas (negativo do mecanismo)";
        c.knobs.hold_cpu_writes = false;
        c.after_satisfied = c.exact_drain = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // Two producers on the same range, the second one after the wait: the later wins.
        Case c{"two_producers", "dois produtores na mesma faixa (P2 depois da espera)"};
        c.entries[1] = {Page(8), 64};
        c.channels = {{Submit(1), Produce(1, Page(8), 32), Simple(OpKind::Wfi),
                       Produce(2, Page(8), 32),
                       Fetch(1, 0, Expected(Page(8), 64).Put(0, ProducerValues(2, 32)))}};
        cases.push_back(c);
    }
    { // Same, but P2 writes a page no earlier producer marked: after the wait the filter
      // is trusted, and P2 is not in it yet. The definition still includes P2.
        Case c{"producer_after_wait_new_page",
               "produtor entre a espera e o fetch, em pagina nova (negativo da regra)"};
        const std::uint32_t va = Page(8) + 992;
        c.entries[1] = {va, 64};
        c.channels = {{Submit(1), Produce(1, va, 32), Simple(OpKind::Wfi), Produce(2, Page(9), 32),
                       Fetch(1, 0,
                             Expected(va, 64)
                                 .Put(0, ProducerValues(1, 32))
                                 .Put(32, ProducerValues(2, 32)))}};
        c.after_satisfied = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // Hyrule Warriors (Lichii 9e33027): compute writes 4 argument words to a page the
      // GPU never wrote; the entry reading them comes right after (sync bit).
        Case c{"small_args_new_page", "argumentos pequenos em pagina nova (sync bit)"};
        c.entries[1] = {Page(10), 8};
        c.channels = {{Submit(1), Produce(1, Page(10), 4), Simple(OpKind::SyncBit),
                       Fetch(1, 0, Expected(Page(10), 8).Put(0, ProducerValues(1, 4)))}};
        cases.push_back(c);
    }
    { // Lichii b13c4c1 (#103): the CPU writes a small structure next to GPU words on the
      // same page; the upload before the copy must not clobber the GPU words.
        Case c{"partial_page_write", "escrita parcial de pagina (CPU ao lado da GPU)"};
        const std::vector<Word> structure{0x51, 0x52, 0x53, 0x54};
        c.entries[1] = {Page(11), 16};
        c.channels = {{Produce(1, Page(11), 8), CpuWrite(Page(11) + 8, structure),
                       Simple(OpKind::Wfi), Submit(1),
                       Fetch(1, 0,
                             Expected(Page(11), 16)
                                 .Put(0, ProducerValues(1, 8))
                                 .Put(8, structure))}};
        cases.push_back(c);
        c.name = "partial_page_write_page_upload";
        c.title = "escrita parcial com upload por pagina (negativo do mecanismo)";
        c.knobs.page_granular_upload = true;
        c.after_satisfied = c.exact_drain = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // The GPU address space is remapped between the producer and a later consumer of
      // the same GPU address: the second entry must see the new physical page only.
        Case c{"remap", "remapeamento do espaco da GPU entre produtor e consumidor"};
        c.entries[1] = {Page(12), 64};
        c.entries[2] = {Page(12), 64};
        c.channels = {{Produce(1, Page(12), 64), Simple(OpKind::Wfi), Submit(1),
                       Fetch(1, 0, Expected(Page(12), 64).Put(0, ProducerValues(1, 64))),
                       Remap(12, 13), Submit(2), Fetch(2, 0, Expected(Page(13), 64))}};
        cases.push_back(c);
        c.name = "remap_ignore_valid_until";
        c.title = "remapeamento ignorando valid_until (negativo do mecanismo)";
        c.knobs.honor_valid_until = false;
        c.after_satisfied = c.exact_drain = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // The syncpoint the consumer waits on is only satisfied at GPU completion; the
      // guest value moves earlier. Waiting on the guest value starts the entry too soon.
        Case c = cases[2];
        c.name = "other_channel_guest_value";
        c.title = "acquire de syncpoint pelo valor do guest (negativo do mecanismo)";
        c.knobs.acquire_uses_guest_value = true;
        c.after_satisfied = c.exact_drain = Expect::FailInSomeForcedSchedule;
        cases.push_back(c);
    }
    { // A value that never arrives: channel 0 ends suspended with a diagnostic and its
      // entry is not executed; channel 1 keeps running meanwhile.
        Case c{"never_arrives", "valor que nunca chega (timeout -> suspend)"};
        c.entries[1] = {Page(14), 16};
        c.entries[2] = {Page(15), 16};
        const Satisfier client{SatisfierKind::Host1xClient, 7};
        c.channels = {
            {Submit(1), SyncptAcquire(2, 5, client, std::chrono::milliseconds(2)),
             Fetch(1, 0, Expected(Page(14), 16))},
            {Produce(1, Page(15), 16), Simple(OpKind::Wfi), Submit(2),
             Fetch(2, 0, Expected(Page(15), 16).Put(0, ProducerValues(1, 16)))}};
        c.expect_suspended = {0};
        c.expect_not_fetched = {1};
        cases.push_back(c);
        c.name = "never_arrives_continue";
        c.title = "valor que nunca chega com on_timeout=Continue (opcao de compatibilidade)";
        c.channels[0][1].on_timeout = OnTimeout::Continue;
        c.expect_suspended.clear();
        c.expect_not_fetched.clear();
        c.expect_continued = {0};
        cases.push_back(c);
    }
    return cases;
}

struct Tally {
    std::uint64_t runs = 0;
    std::uint64_t failures = 0;
    std::uint64_t missed = 0;
    std::uint64_t early = 0;
    std::string first_failure;
    std::string diagnostic;
};

const char* ExpectName(Expect e) {
    switch (e) {
    case Expect::Pass:
        return "pass";
    case Expect::FailInSomeForcedSchedule:
        return "must fail";
    case Expect::Info:
        return "info";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t random_runs = 2000;
    std::uint64_t forced_runs = 200;
    std::uint64_t seed = 1;
    const char* only = nullptr;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--random") && i + 1 < argc) {
            random_runs = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--forced") && i + 1 < argc) {
            forced_runs = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--case") && i + 1 < argc) {
            only = argv[++i];
        } else if (!std::strcmp(argv[i], "-v")) {
            verbose = true;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--random N] [--forced N] [--seed S] [--case NAME] [-v]\n",
                         argv[0]);
            return 2;
        }
    }

    const contracts::AfterSatisfiedDependency after_satisfied;
    const contracts::ExactDrain exact_drain;
    const contracts::NeverWait never_wait;
    const contracts::VisibilityRule* rules[] = {&after_satisfied, &exact_drain, &never_wait};
    const Schedule schedules[] = {Schedule::Random, Schedule::BackLags, Schedule::GpuLags,
                                  Schedule::BothLag, Schedule::Eager};

    std::printf("random runs per schedule: %llu, forced runs per schedule: %llu, seed %llu\n\n",
                static_cast<unsigned long long>(random_runs),
                static_cast<unsigned long long>(forced_runs),
                static_cast<unsigned long long>(seed));
    std::printf("%-30s %-28s %-9s | %-11s %-11s %-11s %-11s %-11s | %s\n", "case", "rule",
                "expect", "random", "back-lags", "gpu-lags", "both-lag", "eager", "verdict");

    bool all_ok = true;
    std::uint64_t run_seed = seed * 1000003;
    for (const Case& c : BuildCases()) {
        if (only && c.name != only) {
            continue;
        }
        for (const contracts::VisibilityRule* rule : rules) {
            const Expect expect = rule == &after_satisfied ? c.after_satisfied
                                  : rule == &exact_drain   ? c.exact_drain
                                                           : Expect::Info;
            Tally tallies[5];
            for (int s = 0; s < 5; ++s) {
                const std::uint64_t n = s == 0 ? random_runs : forced_runs;
                for (std::uint64_t i = 0; i < n; ++i) {
                    const RunResult r = RunCase(c, *rule, schedules[s], ++run_seed);
                    Tally& t = tallies[s];
                    ++t.runs;
                    t.missed += r.missed_writes;
                    t.early += r.early_completion_observed ? 1 : 0;
                    if (!r.ok) {
                        if (t.failures++ == 0) {
                            t.first_failure = r.failure;
                        }
                    }
                    if (t.diagnostic.empty() && !r.suspend_diagnostic.empty()) {
                        t.diagnostic = r.suspend_diagnostic;
                    }
                    if (t.diagnostic.empty() && !r.missed_diagnostic.empty()) {
                        t.diagnostic = "missed write: " + r.missed_diagnostic;
                    }
                }
            }
            std::uint64_t total_failures = 0;
            std::uint64_t forced_failures = 0;
            for (int s = 0; s < 5; ++s) {
                total_failures += tallies[s].failures;
                forced_failures += s == 0 ? 0 : tallies[s].failures;
            }
            const char* verdict = "info";
            if (expect == Expect::Pass) {
                verdict = total_failures == 0 ? "OK" : "UNEXPECTED FAIL";
            } else if (expect == Expect::FailInSomeForcedSchedule) {
                verdict = forced_failures > 0 ? "OK (caught)" : "NOT CAUGHT";
            }
            const bool ok = expect == Expect::Info || verdict[0] == 'O';
            all_ok = all_ok && ok;
            char cells[5][16];
            for (int s = 0; s < 5; ++s) {
                std::snprintf(cells[s], sizeof(cells[s]), "%llu/%llu",
                              static_cast<unsigned long long>(tallies[s].failures),
                              static_cast<unsigned long long>(tallies[s].runs));
            }
            std::printf("%-30s %-28s %-9s | %-11s %-11s %-11s %-11s %-11s | %s\n", c.name.c_str(),
                        rule->Name(), ExpectName(expect), cells[0], cells[1], cells[2], cells[3],
                        cells[4], verdict);
            if (verbose || !ok || (total_failures > 0 && rule != &never_wait)) {
                for (int s = 0; s < 5; ++s) {
                    if (!tallies[s].first_failure.empty()) {
                        std::printf("    %-9s first failure: %s\n", ScheduleName(schedules[s]),
                                    tallies[s].first_failure.c_str());
                        break;
                    }
                }
            }
            if (verbose) {
                for (int s = 0; s < 5; ++s) {
                    if (!tallies[s].diagnostic.empty()) {
                        std::printf("    %-9s diagnostic: %s\n", ScheduleName(schedules[s]),
                                    tallies[s].diagnostic.c_str());
                    }
                    if (tallies[s].missed || tallies[s].early) {
                        std::printf("    %-9s missed writes: %llu, early completion seen in %llu runs\n",
                                    ScheduleName(schedules[s]),
                                    static_cast<unsigned long long>(tallies[s].missed),
                                    static_cast<unsigned long long>(tallies[s].early));
                    }
                }
            }
        }
    }
    std::printf("\n%s\n", all_ok ? "ALL EXPECTATIONS MET" : "EXPECTATIONS NOT MET");
    return all_ok ? 0 : 1;
}
