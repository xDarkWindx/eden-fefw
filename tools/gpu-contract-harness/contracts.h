// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Interfaces of the memory and synchronisation contracts for command list fetch
// (docs/architecture-plan.md 2.2 and 13.2, docs/gpu-produced-data-plan.md 4.2).
//
// Self-contained on purpose: nothing here includes src/, so the harness builds with
// plain g++ and the same header can later move to src/video_core/ (task T3) unchanged.
// The consumer (ListFetchConsumer) only talks to the three interfaces below; the
// question "must this entry wait before it reads produced words?" is answered in one
// place only, the VisibilityRule, so that a broken hypothesis is replaced locally.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace contracts {

using Word = std::uint32_t;
// Position in the front -> back ring. Everything pushed with a lower position is
// "before" in stream order.
using Seq = std::uint64_t;

// Half-open range of guest physical word addresses. Physical, not GPU virtual, so a
// remap of the GPU address space cannot make a transfer describe the wrong bytes.
struct PhysRange {
    std::uint32_t begin;
    std::uint32_t end;
    bool Overlaps(const PhysRange& o) const {
        return begin < o.end && o.begin < end;
    }
};

// ---------------------------------------------------------------------------
// Synchronisation contract
// ---------------------------------------------------------------------------

enum class DependencyKind { Semaphore, Syncpoint, Wfi, SyncBit };

// What happens when a dependency is not satisfied within its limit. The dependency
// stays unsatisfied in every case; only Continue lets the channel run without it, and
// that is an explicit compatibility choice, never the default.
enum class OnTimeout { Suspend, Continue, Abort };

enum class Compare { GreaterEqual, Equal };

struct Condition {
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    Compare compare = Compare::GreaterEqual;
};

enum class SatisfierKind { Channel, Host1xClient, Cpu, BackThread };

struct Satisfier {
    SatisfierKind kind = SatisfierKind::BackThread;
    std::uint32_t id = 0;
};

struct Dependency {
    DependencyKind kind = DependencyKind::Wfi;
    Condition condition{};
    Satisfier satisfier{};
    std::chrono::milliseconds timeout{5000};
    OnTimeout on_timeout = OnTimeout::Suspend;
    // Ring position at which the waiting channel passed the dependency. For WFI and the
    // sync bit this is the marker itself; for acquires it is pushed when the condition
    // holds, so "the back thread passed it" implies it recorded everything the
    // satisfier pushed before satisfying it.
    Seq position = 0;
};

inline bool Holds(const Condition& c, std::uint32_t current) {
    return c.compare == Compare::Equal ? current == c.value : current >= c.value;
}

// ---------------------------------------------------------------------------
// Memory contract
// ---------------------------------------------------------------------------

// A transfer stops being valid when a producer is recorded or the guest CPU writes any
// page of the region it covers, or when the GPU address space is remapped. The consumer
// does not need to know which one happened. The region is the whole one the entry asked
// about, not only what was copied: "the GPU wrote nothing here" is information too, and
// goes stale the same way.
struct ValidUntil {
    std::uint64_t map_epoch = 0;
    std::uint64_t range_epoch = 0;
};

struct Fence {
    std::atomic<bool> signaled{false};
};

struct Transfer {
    // Whole region the entry consulted (every physical word it had at fetch time). The
    // validity protects all of it, including pages the filter discarded.
    std::vector<PhysRange> covered;
    // Part of `covered` that was actually sent for copying (pages the filter says a
    // producer may have written). Empty when the filter found nothing.
    std::vector<PhysRange> requested;
    // Filled by the owner (back thread) when the copy is recorded: the exact GPU written
    // sub-ranges, their words and the producers they came from. Read only after
    // completion_fence is signaled.
    std::vector<PhysRange> ranges;
    std::vector<std::vector<Word>> words;
    std::vector<std::uint32_t> included_producers;
    Seq version = 0;
    std::shared_ptr<Fence> completion_fence;
    ValidUntil valid_until{};
};

class MemoryContract {
public:
    virtual ~MemoryContract() = default;
    // Transfer of the GPU produced words in `covered`, ordered at the caller's current
    // stream position: it includes every producer pushed before and none pushed after.
    // The contract applies its own page filter to decide what to copy, and captures the
    // validity of the whole of `covered` before reading the filter, so a producer recorded
    // in between makes the transfer invalid rather than silently missing.
    virtual std::shared_ptr<Transfer> Request(const std::vector<PhysRange>& covered) = 0;
    virtual void Wait(const Transfer& transfer) = 0;
    virtual bool IsValid(const Transfer& transfer) const = 0;
};

// Harness-only negative control, not part of the contract: a read of the host buffer with no
// copy and no fence. It models a consumer that takes a satisfied syncpoint as proof that the
// bytes are ready (see "syncpoint_trusted_as_ready"): a syncpoint proves order, and only a
// deferred fence also proves completion.
class DirectHostRead {
public:
    virtual ~DirectHostRead() = default;
    virtual std::vector<Word> ReadHostDirect(const std::vector<std::uint32_t>& phys) = 0;
};

// Lets the consumer wait for the back thread without knowing how it is scheduled.
class BackProgress {
public:
    virtual ~BackProgress() = default;
    virtual void WaitBackPassed(Seq position) = 0;
};

// ---------------------------------------------------------------------------
// Visibility rule: the single, swappable place that decides whether an entry waits
// ---------------------------------------------------------------------------

struct FetchContext {
    std::uint32_t channel = 0;
    // Dependencies the channel passed since its previous fetch.
    const std::vector<Dependency>* new_dependencies = nullptr;
    std::uint64_t producers_pushed = 0;   // counted by the front
    std::uint64_t producers_recorded = 0; // published by the back thread
    Seq ring_tail = 0;                    // last position pushed by the front
};

struct WaitPlan {
    bool wait_back = false;
    Seq back_position = 0;
    const char* reason = "none";
};

class VisibilityRule {
public:
    virtual ~VisibilityRule() = default;
    virtual const char* Name() const = 0;
    virtual WaitPlan BeforeFetch(const FetchContext& ctx) const = 0;
};

// The hypothesis of docs/gpu-produced-data-plan.md 4.2 item 2: a producer is only
// visible to the fetch after a satisfied dependency. On the first entry after a new
// dependency, wait for the back thread to pass it; then the page filter is trusted.
class AfterSatisfiedDependency final : public VisibilityRule {
public:
    const char* Name() const override {
        return "after-satisfied-dependency";
    }
    WaitPlan BeforeFetch(const FetchContext& ctx) const override {
        if (ctx.new_dependencies == nullptr || ctx.new_dependencies->empty()) {
            return {};
        }
        Seq position = 0;
        for (const Dependency& dep : *ctx.new_dependencies) {
            position = std::max(position, dep.position);
        }
        return {true, position, "new dependency"};
    }
};

// Conservative: while any producer is still in the ring, wait for the back thread to
// reach the entry (all earlier work recorded), so the filter is complete.
class ExactDrain final : public VisibilityRule {
public:
    const char* Name() const override {
        return "exact-drain";
    }
    WaitPlan BeforeFetch(const FetchContext& ctx) const override {
        if (ctx.producers_pushed > ctx.producers_recorded) {
            return {true, ctx.ring_tail, "producer in the ring"};
        }
        return {};
    }
};

// Harness-only negative control: never waits beyond what the synchronisation contract
// already enforces. It is NOT a model of what the emulator does today.
class NeverWait final : public VisibilityRule {
public:
    const char* Name() const override {
        return "never-wait (control)";
    }
    WaitPlan BeforeFetch(const FetchContext&) const override {
        return {};
    }
};

// ---------------------------------------------------------------------------
// Consumer: command list fetch, built only on the three interfaces above
// ---------------------------------------------------------------------------

struct EntryWords {
    std::vector<Word> words;
    std::shared_ptr<const Transfer> transfer;
    WaitPlan plan{};
    bool reused_transfer = false;
};

class ListFetchConsumer {
public:
    // `trust_direct`, when set, makes the consumer skip the transfer and its fence and read the
    // host buffer directly after the visibility rule's wait (negative control only).
    ListFetchConsumer(const VisibilityRule& rule, MemoryContract& memory, BackProgress& back,
                      bool honor_valid_until = true, DirectHostRead* trust_direct = nullptr)
        : rule_{rule}, memory_{memory}, back_{back}, honor_valid_until_{honor_valid_until},
          trust_direct_{trust_direct} {}

    // `snapshot` holds the words captured at submission, `phys` the physical address each
    // of them has now (translated at fetch time).
    EntryWords Fetch(const FetchContext& ctx, std::uint32_t va, const std::vector<Word>& snapshot,
                     const std::vector<std::uint32_t>& phys) {
        EntryWords out;
        out.plan = rule_.BeforeFetch(ctx);
        if (out.plan.wait_back) {
            back_.WaitBackPassed(out.plan.back_position);
        }

        if (trust_direct_ != nullptr) {
            out.words = trust_direct_->ReadHostDirect(phys);
            out.transfer = std::make_shared<Transfer>();
            return out;
        }

        const Key key{va, static_cast<std::uint32_t>(snapshot.size())};
        const std::vector<std::uint32_t>* used_phys = &phys;
        std::shared_ptr<const Transfer> transfer;
        // A cached transfer is reused only if the rule asked for no wait and the memory
        // contract says it still holds. The cache also remembers the translation it was
        // made with, which is exactly what goes wrong when validity is ignored.
        if (auto it = cache_.find(key); it != cache_.end() && !out.plan.wait_back &&
                                        (!honor_valid_until_ || memory_.IsValid(*it->second.first))) {
            transfer = it->second.first;
            used_phys = &it->second.second;
            out.reused_transfer = true;
        } else {
            std::vector<PhysRange> covered;
            for (std::uint32_t pa : phys) {
                if (!covered.empty() && covered.back().end == pa) {
                    ++covered.back().end;
                } else {
                    covered.push_back({pa, pa + 1});
                }
            }
            auto fresh = memory_.Request(covered);
            memory_.Wait(*fresh);
            transfer = fresh;
            cache_[key] = {fresh, phys};
            used_phys = &cache_[key].second;
        }

        out.words = snapshot;
        for (std::size_t i = 0; i < out.words.size(); ++i) {
            const std::uint32_t pa = (*used_phys)[i];
            for (std::size_t r = 0; r < transfer->ranges.size(); ++r) {
                const PhysRange& range = transfer->ranges[r];
                if (pa >= range.begin && pa < range.end) {
                    out.words[i] = transfer->words[r][pa - range.begin];
                }
            }
        }
        out.transfer = std::move(transfer);
        return out;
    }

private:
    using Key = std::pair<std::uint32_t, std::uint32_t>;
    const VisibilityRule& rule_;
    MemoryContract& memory_;
    BackProgress& back_;
    bool honor_valid_until_;
    DirectHostRead* trust_direct_;
    std::map<Key, std::pair<std::shared_ptr<const Transfer>, std::vector<std::uint32_t>>> cache_;
};

} // namespace contracts
