// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "model.h"

#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace harness {

using contracts::Dependency;
using contracts::DependencyKind;
using contracts::OnTimeout;
using contracts::PhysRange;
using contracts::SatisfierKind;
using contracts::Transfer;
using Clock = std::chrono::steady_clock;

const char* ScheduleName(Schedule s) {
    switch (s) {
    case Schedule::Random:
        return "random";
    case Schedule::BackLags:
        return "back-lags";
    case Schedule::GpuLags:
        return "gpu-lags";
    case Schedule::BothLag:
        return "both-lag";
    case Schedule::Eager:
        return "eager";
    }
    return "?";
}

const char* FailureKindName(FailureKind kind) {
    switch (kind) {
    case FailureKind::WordMismatch:
        return "WordMismatch";
    case FailureKind::DependencyViolated:
        return "DependencyViolated";
    case FailureKind::UnexpectedSuspend:
        return "UnexpectedSuspend";
    case FailureKind::MissingSuspend:
        return "MissingSuspend";
    case FailureKind::ContinueMismatch:
        return "ContinueMismatch";
    case FailureKind::Timeout:
        return "Timeout";
    case FailureKind::Hang:
        return "Hang";
    case FailureKind::CaseError:
        return "CaseError";
    }
    return "?";
}

std::string FailureKindsName(std::uint32_t kinds) {
    std::string out;
    for (std::uint32_t bit = 1; bit != 0 && bit <= kinds; bit <<= 1) {
        if (kinds & bit) {
            out += (out.empty() ? "" : "+");
            out += FailureKindName(static_cast<FailureKind>(bit));
        }
    }
    return out.empty() ? "-" : out;
}

namespace {

constexpr Seq kUnlimited = std::numeric_limits<Seq>::max();
// A run that has not finished by then is a harness hang, reported as a failure.
constexpr auto kWatchdog = std::chrono::seconds(20);

std::vector<PhysRange> Coalesce(const std::vector<std::uint32_t>& phys) {
    std::vector<PhysRange> out;
    for (std::uint32_t pa : phys) {
        if (!out.empty() && out.back().end == pa) {
            ++out.back().end;
        } else {
            out.push_back({pa, pa + 1});
        }
    }
    return out;
}

bool AnyOverlap(const std::vector<PhysRange>& a, const std::vector<PhysRange>& b) {
    for (const PhysRange& x : a) {
        for (const PhysRange& y : b) {
            if (x.Overlaps(y)) {
                return true;
            }
        }
    }
    return false;
}

// Single producer, single consumer ring with a fixed capacity, like the front -> back
// ring of the split.
template <typename T, std::size_t N>
class SpscRing {
public:
    bool TryPush(T&& value) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == N) {
            return false;
        }
        slots_[t % N] = std::move(value);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    T* Peek() {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &slots_[h % N];
    }
    void Pop() {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        slots_[h % N] = T{};
        head_.store(h + 1, std::memory_order_release);
    }

private:
    std::array<T, N> slots_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

enum class RingKind { None, Produce, Marker, SyncptIncr, SemRelease, Copy, FetchMarker, Stop };

struct RingItem {
    RingKind kind = RingKind::None;
    Seq seq = 0;
    std::uint32_t id = 0;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::vector<PhysRange> ranges;
    std::vector<Word> values;
    std::shared_ptr<Transfer> transfer;
    std::vector<std::uint32_t> included;
};

enum class GpuKind { None, Write, Copy, SyncptIncr, SemRelease, Stop };

struct GpuCmd {
    GpuKind kind = GpuKind::None;
    Seq seq = 0;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::vector<std::uint32_t> phys; // Write: one address per value
    std::vector<Word> values;
    std::shared_ptr<Transfer> transfer;
};

class Jitter {
public:
    explicit Jitter(std::uint64_t seed) : rng_{seed} {}
    void Maybe() {
        const std::uint32_t r = rng_() % 1000;
        if (r < 20) {
            std::this_thread::sleep_for(std::chrono::microseconds(1 + rng_() % 100));
        } else if (r < 500) {
            const std::uint32_t spins = rng_() % 2000;
            for (std::uint32_t i = 0; i < spins; ++i) {
                sink_ = sink_ + i;
            }
        } else if (r < 600) {
            std::this_thread::yield();
        }
    }

private:
    std::mt19937_64 rng_;
    volatile std::uint32_t sink_ = 0;
};

struct ProducerRecord {
    std::uint32_t id;
    Seq seq;
    std::vector<PhysRange> ranges;
};

struct ChannelState {
    std::size_t pc = 0;
    bool done = false;
    bool waiting = false;
    bool suspended = false;
    bool continued = false;
    Clock::time_point wait_start{};
    std::vector<Dependency> new_dependencies;
};

struct EntryState {
    std::vector<Word> snapshot;
    std::vector<std::uint32_t> phys_at_submit;
    bool submitted = false;
    bool fetched = false;
    std::size_t fetches_left = 0;
};

struct HeldWrite {
    std::vector<std::uint32_t> phys;
    std::vector<std::uint32_t> entries;
};

class Model;

class ModelMemory final : public contracts::MemoryContract {
public:
    explicit ModelMemory(Model& m) : m_{m} {}
    std::shared_ptr<Transfer> Request(const std::vector<PhysRange>& ranges) override;
    void Wait(const Transfer& transfer) override;
    bool IsValid(const Transfer& transfer) const override;

private:
    Model& m_;
};

class ModelBack final : public contracts::BackProgress {
public:
    explicit ModelBack(Model& m) : m_{m} {}
    void WaitBackPassed(Seq position) override;

private:
    Model& m_;
};

class Model {
public:
    Model(const Case& c, const contracts::VisibilityRule& rule, Schedule schedule,
          std::uint64_t seed)
        : case_{c}, rule_{rule}, schedule_{schedule}, knobs_{c.knobs}, seed_{seed},
          front_jitter_{seed * 3 + 1}, memory_{*this}, back_progress_{*this},
          consumer_{rule, memory_, back_progress_, c.knobs.honor_valid_until} {
        guest_.resize(kWords);
        host_.resize(kWords);
        cpu_dirty_.assign(kWords, 0);
        gpu_modified_.assign(kWords, 0);
        for (std::uint32_t pa = 0; pa < kWords; ++pa) {
            guest_[pa] = Original(pa);
            host_[pa] = Original(pa);
        }
        for (std::uint32_t p = 0; p < kPages; ++p) {
            page_table_[p] = p;
            gpu_written_[p].store(0);
            page_epoch_[p].store(0);
        }
        const bool back_lags = schedule == Schedule::BackLags || schedule == Schedule::BothLag;
        const bool gpu_lags = schedule == Schedule::GpuLags || schedule == Schedule::BothLag;
        back_allowed_.store(back_lags ? 0 : kUnlimited);
        gpu_allowed_.store(gpu_lags ? 0 : kUnlimited);
        for (const auto& [id, entry] : c.entries) {
            entries_[id];
        }
        for (const auto& program : c.channels) {
            for (const Op& op : program) {
                if (op.kind == OpKind::Fetch) {
                    ++entries_[op.id].fetches_left;
                }
            }
        }
        channels_.resize(c.channels.size());
        deadline_ = Clock::now() + kWatchdog;
    }

    RunResult Run() {
        std::thread back([this] { BackLoop(); });
        std::thread gpu([this] { GpuLoop(); });
        FrontLoop();
        RingItem stop;
        stop.kind = RingKind::Stop;
        Push(std::move(stop));
        back_allowed_.store(kUnlimited, std::memory_order_release);
        gpu_allowed_.store(kUnlimited, std::memory_order_release);
        back.join();
        gpu.join();
        Finish();
        return result_;
    }

    // ----- front side helpers used by the contracts -----

    void RaiseBack(Seq position) {
        Seq cur = back_allowed_.load(std::memory_order_relaxed);
        while (cur < position &&
               !back_allowed_.compare_exchange_weak(cur, position, std::memory_order_release)) {
        }
    }
    void RaiseGpu(Seq position) {
        Seq cur = gpu_allowed_.load(std::memory_order_relaxed);
        while (cur < position &&
               !gpu_allowed_.compare_exchange_weak(cur, position, std::memory_order_release)) {
        }
    }

    bool Hung() {
        if (aborted_.load(std::memory_order_acquire)) {
            return true;
        }
        if (Clock::now() > deadline_) {
            aborted_.store(true, std::memory_order_release);
            Fail(FailureKind::Hang, "harness hang: watchdog expired");
            return true;
        }
        return false;
    }

    void WaitBackPassed(Seq position) {
        RaiseBack(position);
        while (back_pos_.load(std::memory_order_acquire) < position && !Hung()) {
            std::this_thread::yield();
        }
    }

    std::shared_ptr<Transfer> Request(const std::vector<PhysRange>& covered) {
        auto transfer = std::make_shared<Transfer>();
        transfer->covered = covered;
        transfer->completion_fence = std::make_shared<contracts::Fence>();
        // Validity first, filter second. The back thread sets a page's filter bit before
        // it bumps the page epoch, so either this read sees the new epoch (and then the
        // filter read below sees the bit), or the epoch moves later and the transfer is
        // invalid at its next use.
        transfer->valid_until = CurrentValidity(covered);
        for (const PhysRange& r : covered) {
            for (std::uint32_t pa = r.begin; pa < r.end; ++pa) {
                if (gpu_written_[pa / kPageWords].load(std::memory_order_acquire) == 0) {
                    continue;
                }
                auto& req = transfer->requested;
                if (!req.empty() && req.back().end == pa) {
                    ++req.back().end;
                } else {
                    req.push_back({pa, pa + 1});
                }
            }
        }
        if (transfer->requested.empty()) {
            transfer->version = tail_seq_;
            transfer->completion_fence->signaled.store(true, std::memory_order_release);
            return transfer;
        }
        RingItem item;
        item.kind = RingKind::Copy;
        item.transfer = transfer;
        transfer->version = Push(std::move(item));
        return transfer;
    }

    void Wait(const Transfer& transfer) {
        RaiseBack(transfer.version);
        RaiseGpu(transfer.version);
        while (!transfer.completion_fence->signaled.load(std::memory_order_acquire) && !Hung()) {
            std::this_thread::yield();
        }
    }

    bool IsValid(const Transfer& transfer) const {
        const contracts::ValidUntil now = CurrentValidity(transfer.covered);
        return now.map_epoch == transfer.valid_until.map_epoch &&
               now.range_epoch == transfer.valid_until.range_epoch;
    }

private:
    contracts::ValidUntil CurrentValidity(const std::vector<PhysRange>& ranges) const {
        contracts::ValidUntil v;
        v.map_epoch = map_epoch_.load(std::memory_order_acquire);
        std::uint32_t last_page = kPages;
        for (const PhysRange& r : ranges) {
            for (std::uint32_t page = r.begin / kPageWords; page <= (r.end - 1) / kPageWords;
                 ++page) {
                if (page == last_page) {
                    continue;
                }
                last_page = page;
                v.range_epoch += page_epoch_[page].load(std::memory_order_acquire);
            }
        }
        return v;
    }

    void Fail(FailureKind kind, const std::string& why) {
        std::lock_guard lock{result_mutex_};
        result_.failure_kinds |= static_cast<std::uint32_t>(kind);
        if (result_.ok) {
            result_.ok = false;
            result_.failure = why;
        }
    }

    std::vector<std::uint32_t> Translate(std::uint32_t va, std::uint32_t len) const {
        std::vector<std::uint32_t> phys(len);
        for (std::uint32_t i = 0; i < len; ++i) {
            const std::uint32_t a = va + i;
            phys[i] = page_table_[a / kPageWords] * kPageWords + a % kPageWords;
        }
        return phys;
    }

    // Front: push one item into the ring, return its position.
    Seq Push(RingItem&& item) {
        item.seq = ++tail_seq_;
        const Seq seq = item.seq;
        const bool counts_as_producer = item.kind == RingKind::Produce;
        while (!ring_.TryPush(std::move(item))) {
            // Ring full: the back thread has to make room, whatever the schedule says.
            // After a hang the Stop item must still get through, so the back may run freely.
            RaiseBack(Hung() ? kUnlimited : seq);
            std::this_thread::yield();
        }
        if (counts_as_producer) {
            ++producers_pushed_;
        }
        if (schedule_ == Schedule::Eager) {
            WaitBackPassed(seq);
            while (gpu_executed_.load(std::memory_order_acquire) <
                       gpu_enqueued_.load(std::memory_order_acquire) &&
                   !Hung()) {
                std::this_thread::yield();
            }
        }
        return seq;
    }

    bool DependencySatisfied(const Op& op, std::string* why) const {
        if (op.kind == OpKind::SemAcquire) {
            const std::uint32_t cur = semaphore_[op.index].load(std::memory_order_acquire);
            if (why) {
                *why = "semaphore[" + std::to_string(op.index) + "] >= " +
                       std::to_string(op.value) + ", current " + std::to_string(cur);
            }
            return contracts::Holds({op.index, op.value}, cur);
        }
        const std::uint32_t host = host_syncpoint_[op.index].load(std::memory_order_acquire);
        const std::uint32_t guest = guest_syncpoint_[op.index].load(std::memory_order_acquire);
        if (why) {
            *why = "syncpoint[" + std::to_string(op.index) + "] >= " + std::to_string(op.value) +
                   ", host value " + std::to_string(host) + ", guest value " +
                   std::to_string(guest);
        }
        return contracts::Holds({op.index, op.value},
                                knobs_.acquire_uses_guest_value ? guest : host);
    }

    static const char* SatisfierName(const contracts::Satisfier& s, std::string* storage) {
        switch (s.kind) {
        case SatisfierKind::Channel:
            *storage = "channel " + std::to_string(s.id);
            break;
        case SatisfierKind::Host1xClient:
            *storage = "host1x client " + std::to_string(s.id);
            break;
        case SatisfierKind::Cpu:
            *storage = "guest CPU";
            break;
        case SatisfierKind::BackThread:
            *storage = "back thread";
            break;
        }
        return storage->c_str();
    }

    void PassDependency(ChannelState& ch, const Op& op) {
        RingItem marker;
        marker.kind = RingKind::Marker;
        Dependency dep;
        switch (op.kind) {
        case OpKind::Wfi:
            dep.kind = DependencyKind::Wfi;
            break;
        case OpKind::SyncBit:
            dep.kind = DependencyKind::SyncBit;
            break;
        case OpKind::SemAcquire:
            dep.kind = DependencyKind::Semaphore;
            break;
        default:
            dep.kind = DependencyKind::Syncpoint;
            break;
        }
        dep.condition = {op.index, op.value};
        dep.satisfier = op.satisfier;
        dep.timeout = op.timeout;
        dep.on_timeout = op.on_timeout;
        dep.position = Push(std::move(marker));
        ch.new_dependencies.push_back(dep);
    }

    // Returns false when the channel parks on this op.
    bool Execute(std::uint32_t channel, ChannelState& ch, const Op& op) {
        switch (op.kind) {
        case OpKind::Produce: {
            RingItem item;
            item.kind = RingKind::Produce;
            item.id = op.id;
            // Resolved at push time: the address space the channel sees at that point of
            // its stream. See README, "what the model does not cover".
            const auto phys = Translate(op.va, static_cast<std::uint32_t>(op.values.size()));
            item.ranges = Coalesce(phys);
            item.values = op.values;
            Push(std::move(item));
            return true;
        }
        case OpKind::Wfi:
        case OpKind::SyncBit:
            PassDependency(ch, op);
            return true;
        case OpKind::SyncptIncr: {
            // Fast signals the guest value before the GPU gets there (SignalFenceCoalesced,
            // IncrementGuest); the host value follows at GPU completion (IncrementHost).
            guest_syncpoint_[op.index].fetch_add(1, std::memory_order_acq_rel);
            RingItem item;
            item.kind = RingKind::SyncptIncr;
            item.index = op.index;
            Push(std::move(item));
            return true;
        }
        case OpKind::SemRelease: {
            RingItem item;
            item.kind = RingKind::SemRelease;
            item.index = op.index;
            item.value = op.value;
            Push(std::move(item));
            return true;
        }
        case OpKind::SyncptAcquire:
        case OpKind::SemAcquire: {
            std::string why;
            if (DependencySatisfied(op, &why)) {
                if (ch.suspended) {
                    result_.suspend_diagnostic += " [resumed later: " + why + "]";
                    ch.suspended = false;
                }
                ch.waiting = false;
                PassDependency(ch, op);
                return true;
            }
            if (!ch.waiting) {
                ch.waiting = true;
                ch.wait_start = Clock::now();
            }
            if (!ch.suspended && !ch.continued && Clock::now() - ch.wait_start > op.timeout) {
                std::string who;
                std::ostringstream diag;
                diag << "channel " << channel << " waited "
                     << std::chrono::duration_cast<std::chrono::milliseconds>(op.timeout).count()
                     << " ms for " << why << "; satisfier: " << SatisfierName(op.satisfier, &who)
                     << "; next op index " << ch.pc + 1;
                switch (op.on_timeout) {
                case OnTimeout::Suspend:
                    ch.suspended = true;
                    result_.suspended = true;
                    result_.suspend_diagnostic = "suspended: " + diag.str();
                    break;
                case OnTimeout::Continue:
                    // Explicit compatibility option: run without the dependency, and say so.
                    ch.continued = true;
                    ch.waiting = false;
                    result_.suspend_diagnostic = "continued without dependency: " + diag.str();
                    PassDependency(ch, op);
                    return true;
                case OnTimeout::Abort:
                    Fail(FailureKind::Timeout, "aborted: " + diag.str());
                    aborted_.store(true, std::memory_order_release);
                    break;
                }
            }
            return false;
        }
        case OpKind::CpuWrite: {
            std::vector<std::uint32_t> phys(op.values.size());
            for (std::size_t i = 0; i < phys.size(); ++i) {
                phys[i] = op.pa + static_cast<std::uint32_t>(i);
            }
            HeldWrite held;
            if (knobs_.hold_cpu_writes) {
                const auto ranges = Coalesce(phys);
                for (const auto& [id, e] : entries_) {
                    if (e.submitted && e.fetches_left > 0 &&
                        AnyOverlap(ranges, Coalesce(e.phys_at_submit))) {
                        held.entries.push_back(id);
                    }
                }
            }
            {
                std::lock_guard lock{guest_mutex_};
                for (std::size_t i = 0; i < phys.size(); ++i) {
                    guest_[phys[i]] = op.values[i];
                    if (held.entries.empty()) {
                        cpu_dirty_[phys[i]] = 1;
                    }
                }
            }
            for (std::uint32_t page = op.pa / kPageWords;
                 page <= (op.pa + static_cast<std::uint32_t>(phys.size()) - 1) / kPageWords;
                 ++page) {
                page_epoch_[page].fetch_add(1, std::memory_order_acq_rel);
            }
            if (!held.entries.empty()) {
                held.phys = std::move(phys);
                held_.push_back(std::move(held));
            }
            return true;
        }
        case OpKind::GuestObserve: {
            const std::uint32_t guest = guest_syncpoint_[op.index].load(std::memory_order_acquire);
            const std::uint32_t host = host_syncpoint_[op.index].load(std::memory_order_acquire);
            if (guest < op.value) {
                Fail(FailureKind::CaseError, "case error: the guest cannot observe syncpoint " + std::to_string(op.index));
            }
            if (host < op.value) {
                result_.early_completion_observed = true;
            }
            return true;
        }
        case OpKind::Remap:
            page_table_[op.page_va] = op.page_pa;
            map_epoch_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        case OpKind::Submit: {
            const Entry& entry = case_.entries.at(op.id);
            EntryState& state = entries_[op.id];
            state.phys_at_submit = Translate(entry.va, entry.len);
            state.snapshot.resize(entry.len);
            std::lock_guard lock{guest_mutex_};
            for (std::uint32_t i = 0; i < entry.len; ++i) {
                state.snapshot[i] = guest_[state.phys_at_submit[i]];
            }
            state.submitted = true;
            return true;
        }
        case OpKind::Fetch:
            DoFetch(channel, ch, op);
            return true;
        }
        return true;
    }

    void DoFetch(std::uint32_t channel, ChannelState& ch, const Op& op) {
        const Entry& entry = case_.entries.at(op.id);
        EntryState& state = entries_[op.id];
        if (op.require_host_syncpoint) {
            const std::uint32_t host = host_syncpoint_[op.index].load(std::memory_order_acquire);
            if (host < op.value) {
                Fail(FailureKind::DependencyViolated, "entry " + std::to_string(op.id) + " started before its dependency held: "
                     "host syncpoint[" + std::to_string(op.index) + "] = " + std::to_string(host) +
                     " < " + std::to_string(op.value));
            }
        }
        const std::uint32_t va = entry.va + op.offset;
        const auto phys = Translate(va, op.len);
        const std::vector<Word> snapshot(state.snapshot.begin() + op.offset,
                                         state.snapshot.begin() + op.offset + op.len);
        contracts::FetchContext ctx;
        ctx.channel = channel;
        ctx.new_dependencies = &ch.new_dependencies;
        ctx.producers_pushed = producers_pushed_;
        ctx.producers_recorded = producers_recorded_.load(std::memory_order_acquire);
        ctx.ring_tail = tail_seq_;
        auto words = consumer_.Fetch(ctx, va, snapshot, phys);
        ch.new_dependencies.clear();

        // Diagnostic of the first-write blind spot: at the fetch position the back thread
        // checks whether a producer pushed earlier wrote these words without being included.
        RingItem marker;
        marker.kind = RingKind::FetchMarker;
        marker.id = op.id;
        marker.ranges = Coalesce(phys);
        marker.included = words.transfer->included_producers;
        Push(std::move(marker));

        for (std::uint32_t i = 0; i < op.len; ++i) {
            if (words.words[i] != op.expected[i]) {
                std::ostringstream s;
                s << "entry " << op.id << " word " << (op.offset + i) << ": got 0x" << std::hex
                  << words.words[i] << ", expected 0x" << op.expected[i] << std::dec
                  << " (wait: " << words.plan.reason << ", transfer v" << words.transfer->version
                  << ", " << words.transfer->ranges.size() << " ranges, producers {";
                for (std::uint32_t p : words.transfer->included_producers) {
                    s << ' ' << p;
                }
                s << " }" << (words.reused_transfer ? ", reused cached transfer" : "") << ")";
                Fail(FailureKind::WordMismatch, s.str());
                break;
            }
        }
        state.fetched = true;
        if (--state.fetches_left == 0) {
            ReleaseHeldWrites();
        }
    }

    void ReleaseHeldWrites() {
        std::lock_guard lock{guest_mutex_};
        for (auto it = held_.begin(); it != held_.end();) {
            bool all_done = true;
            for (std::uint32_t id : it->entries) {
                all_done = all_done && entries_[id].fetches_left == 0;
            }
            if (!all_done) {
                ++it;
                continue;
            }
            for (std::uint32_t pa : it->phys) {
                cpu_dirty_[pa] = 1;
            }
            it = held_.erase(it);
        }
    }

    void FrontLoop() {
        while (!Hung()) {
            bool progress = false;
            bool remaining = false;
            bool all_remaining_suspended = true;
            for (std::uint32_t c = 0; c < channels_.size(); ++c) {
                ChannelState& ch = channels_[c];
                const auto& program = case_.channels[c];
                while (!ch.done && !Hung()) {
                    if (ch.pc == program.size()) {
                        ch.done = true;
                        break;
                    }
                    if (schedule_ == Schedule::Random) {
                        front_jitter_.Maybe();
                    }
                    if (!Execute(c, ch, program[ch.pc])) {
                        break;
                    }
                    ++ch.pc;
                    progress = true;
                }
                if (!ch.done) {
                    remaining = true;
                    all_remaining_suspended = all_remaining_suspended && ch.suspended;
                }
            }
            if (!remaining) {
                return;
            }
            if (!progress) {
                // Every runnable channel is parked: the satisfier may only be waiting for
                // back or GPU progress, so let them run everything pushed so far. A parked
                // channel never blocks the others; they already ran above.
                RaiseBack(tail_seq_);
                RaiseGpu(tail_seq_);
                if (all_remaining_suspended && Idle()) {
                    return; // nothing left that could ever satisfy them
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    bool Idle() const {
        return back_pos_.load(std::memory_order_acquire) == tail_seq_ &&
               gpu_executed_.load(std::memory_order_acquire) ==
                   gpu_enqueued_.load(std::memory_order_acquire);
    }

    // ----- back thread -----

    void PushGpu(GpuCmd&& cmd) {
        while (!gpu_queue_.TryPush(std::move(cmd))) {
            RaiseGpu(cmd.seq);
            std::this_thread::yield();
        }
        gpu_enqueued_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Records the upload of CPU-written words in `ranges`, as the buffer cache does
    // before a use. Exact words by default; whole pages with the negative knob.
    void UploadCpuWrites(Seq seq, const std::vector<PhysRange>& ranges) {
        GpuCmd cmd;
        cmd.kind = GpuKind::Write;
        cmd.seq = seq;
        std::lock_guard lock{guest_mutex_};
        for (const PhysRange& r : ranges) {
            for (std::uint32_t pa = r.begin; pa < r.end; ++pa) {
                if (!cpu_dirty_[pa]) {
                    continue;
                }
                std::uint32_t begin = pa;
                std::uint32_t end = pa + 1;
                if (knobs_.page_granular_upload) {
                    begin = pa / kPageWords * kPageWords;
                    end = begin + kPageWords;
                }
                for (std::uint32_t a = begin; a < end; ++a) {
                    cmd.phys.push_back(a);
                    cmd.values.push_back(guest_[a]);
                    cpu_dirty_[a] = 0;
                    gpu_modified_[a] = 0; // CPU data supersedes what the GPU wrote there
                }
            }
        }
        if (!cmd.phys.empty()) {
            PushGpu(std::move(cmd));
        }
    }

    void BackLoop() {
        Jitter jitter{seed_ * 3 + 2};
        while (true) {
            RingItem* item = ring_.Peek();
            if (item == nullptr || item->seq > back_allowed_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
                continue;
            }
            if (schedule_ == Schedule::Random) {
                jitter.Maybe();
            }
            const Seq seq = item->seq;
            const RingKind kind = item->kind;
            switch (kind) {
            case RingKind::Produce: {
                UploadCpuWrites(seq, item->ranges);
                GpuCmd cmd;
                cmd.kind = GpuKind::Write;
                cmd.seq = seq;
                for (const PhysRange& r : item->ranges) {
                    for (std::uint32_t pa = r.begin; pa < r.end; ++pa) {
                        cmd.phys.push_back(pa);
                        gpu_modified_[pa] = 1;
                    }
                    for (std::uint32_t page = r.begin / kPageWords;
                         page <= (r.end - 1) / kPageWords; ++page) {
                        gpu_written_[page].store(1, std::memory_order_release);
                        page_epoch_[page].fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                cmd.values = std::move(item->values);
                producers_.push_back({item->id, seq, item->ranges});
                PushGpu(std::move(cmd));
                producers_recorded_.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
            case RingKind::Marker:
                break;
            case RingKind::SyncptIncr: {
                GpuCmd cmd;
                cmd.kind = GpuKind::SyncptIncr;
                cmd.seq = seq;
                cmd.index = item->index;
                PushGpu(std::move(cmd));
                break;
            }
            case RingKind::SemRelease: {
                GpuCmd cmd;
                cmd.kind = GpuKind::SemRelease;
                cmd.seq = seq;
                cmd.index = item->index;
                cmd.value = item->value;
                PushGpu(std::move(cmd));
                break;
            }
            case RingKind::Copy: {
                Transfer& t = *item->transfer;
                UploadCpuWrites(seq, t.requested);
                // The owner decides the exact ranges: requested words the GPU wrote.
                for (const PhysRange& r : t.requested) {
                    for (std::uint32_t pa = r.begin; pa < r.end; ++pa) {
                        if (!gpu_modified_[pa]) {
                            continue;
                        }
                        if (!t.ranges.empty() && t.ranges.back().end == pa) {
                            ++t.ranges.back().end;
                        } else {
                            t.ranges.push_back({pa, pa + 1});
                        }
                    }
                }
                for (const ProducerRecord& p : producers_) {
                    if (AnyOverlap(p.ranges, t.ranges)) {
                        t.included_producers.push_back(p.id);
                    }
                }
                GpuCmd cmd;
                cmd.kind = GpuKind::Copy;
                cmd.seq = seq;
                cmd.transfer = item->transfer;
                PushGpu(std::move(cmd));
                break;
            }
            case RingKind::FetchMarker:
                for (const ProducerRecord& p : producers_) {
                    if (p.seq > seq || !AnyOverlap(p.ranges, item->ranges)) {
                        continue;
                    }
                    if (std::find(item->included.begin(), item->included.end(), p.id) ==
                        item->included.end()) {
                        if (missed_writes_ == 0) {
                            missed_diagnostic_ = "producer " + std::to_string(p.id) +
                                                 " (position " + std::to_string(p.seq) +
                                                 ") wrote entry " + std::to_string(item->id) +
                                                 " but was not included";
                        }
                        ++missed_writes_;
                    }
                }
                break;
            case RingKind::Stop: {
                GpuCmd cmd;
                cmd.kind = GpuKind::Stop;
                cmd.seq = seq;
                PushGpu(std::move(cmd));
                break;
            }
            case RingKind::None:
                break;
            }
            ring_.Pop();
            back_pos_.store(seq, std::memory_order_release);
            if (kind == RingKind::Stop) {
                return;
            }
        }
    }

    // ----- GPU thread: the only one touching host_ -----

    void GpuLoop() {
        Jitter jitter{seed_ * 3 + 3};
        while (true) {
            GpuCmd* cmd = gpu_queue_.Peek();
            if (cmd == nullptr || cmd->seq > gpu_allowed_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
                continue;
            }
            if (schedule_ == Schedule::Random) {
                jitter.Maybe();
            }
            const GpuKind kind = cmd->kind;
            switch (kind) {
            case GpuKind::Write:
                for (std::size_t i = 0; i < cmd->phys.size(); ++i) {
                    host_[cmd->phys[i]] = cmd->values[i];
                }
                break;
            case GpuKind::Copy: {
                Transfer& t = *cmd->transfer;
                for (const PhysRange& r : t.ranges) {
                    t.words.emplace_back(host_.begin() + r.begin, host_.begin() + r.end);
                }
                t.completion_fence->signaled.store(true, std::memory_order_release);
                break;
            }
            case GpuKind::SyncptIncr:
                host_syncpoint_[cmd->index].fetch_add(1, std::memory_order_acq_rel);
                break;
            case GpuKind::SemRelease:
                semaphore_[cmd->index].store(cmd->value, std::memory_order_release);
                break;
            case GpuKind::Stop:
            case GpuKind::None:
                break;
            }
            gpu_queue_.Pop();
            gpu_executed_.fetch_add(1, std::memory_order_acq_rel);
            if (kind == GpuKind::Stop) {
                return;
            }
        }
    }

    void Finish() {
        result_.missed_writes = missed_writes_;
        result_.missed_diagnostic = missed_diagnostic_;
        auto contains = [](const std::vector<std::uint32_t>& v, std::uint32_t x) {
            return std::find(v.begin(), v.end(), x) != v.end();
        };
        for (std::uint32_t c = 0; c < channels_.size(); ++c) {
            const ChannelState& ch = channels_[c];
            const bool want_suspended = contains(case_.expect_suspended, c);
            if (want_suspended && !(ch.suspended && !ch.done)) {
                Fail(FailureKind::MissingSuspend, "channel " + std::to_string(c) + " should have been left suspended");
            }
            if (!want_suspended && !ch.done) {
                Fail(FailureKind::UnexpectedSuspend, "channel " + std::to_string(c) + " did not finish: " +
                     result_.suspend_diagnostic);
            }
            if (contains(case_.expect_continued, c) != ch.continued) {
                Fail(FailureKind::ContinueMismatch, "channel " + std::to_string(c) + (ch.continued ? " continued" : " did not continue") +
                     " after the timeout, contrary to the case");
            }
        }
        for (const auto& [id, state] : entries_) {
            if (contains(case_.expect_not_fetched, id) && state.fetched) {
                Fail(FailureKind::DependencyViolated, "entry " + std::to_string(id) + " was executed without its dependency");
            }
        }
    }

    const Case& case_;
    const contracts::VisibilityRule& rule_;
    const Schedule schedule_;
    const ModelKnobs knobs_;
    const std::uint64_t seed_;
    Jitter front_jitter_;
    ModelMemory memory_;
    ModelBack back_progress_;
    contracts::ListFetchConsumer consumer_;
    Clock::time_point deadline_;

    // Guest memory and the CPU-written marks, shared by the front (CPU, snapshots) and the
    // back thread (uploads).
    std::mutex guest_mutex_;
    std::vector<Word> guest_;
    std::vector<std::uint8_t> cpu_dirty_;

    std::vector<Word> host_;                 // GPU thread only
    std::vector<std::uint8_t> gpu_modified_; // back thread only (exact ranges, owner)

    // Page filter: set by the back thread when it records a GPU write.
    std::array<std::atomic<std::uint8_t>, kPages> gpu_written_{};
    std::array<std::atomic<std::uint64_t>, kPages> page_epoch_{};
    std::atomic<std::uint64_t> map_epoch_{0};
    std::array<std::uint32_t, kPages> page_table_{}; // front only

    std::array<std::atomic<std::uint32_t>, kSyncObjects> guest_syncpoint_{};
    std::array<std::atomic<std::uint32_t>, kSyncObjects> host_syncpoint_{};
    std::array<std::atomic<std::uint32_t>, kSyncObjects> semaphore_{};

    SpscRing<RingItem, 256> ring_;
    SpscRing<GpuCmd, 512> gpu_queue_;
    std::atomic<Seq> back_pos_{0};
    std::atomic<Seq> back_allowed_{0};
    std::atomic<Seq> gpu_allowed_{0};
    std::atomic<std::uint64_t> producers_recorded_{0};
    std::atomic<std::uint64_t> gpu_enqueued_{0};
    std::atomic<std::uint64_t> gpu_executed_{0};
    std::atomic<bool> aborted_{false};

    // Front only.
    Seq tail_seq_ = 0;
    std::uint64_t producers_pushed_ = 0;
    std::vector<ChannelState> channels_;
    std::map<std::uint32_t, EntryState> entries_;
    std::vector<HeldWrite> held_;

    // Back only (read after join).
    std::vector<ProducerRecord> producers_;
    std::uint64_t missed_writes_ = 0;
    std::string missed_diagnostic_;

    std::mutex result_mutex_;
    RunResult result_;

    friend class ModelMemory;
    friend class ModelBack;
};

std::shared_ptr<Transfer> ModelMemory::Request(const std::vector<PhysRange>& ranges) {
    return m_.Request(ranges);
}
void ModelMemory::Wait(const Transfer& transfer) {
    m_.Wait(transfer);
}
bool ModelMemory::IsValid(const Transfer& transfer) const {
    return m_.IsValid(transfer);
}
void ModelBack::WaitBackPassed(Seq position) {
    m_.WaitBackPassed(position);
}

} // namespace

RunResult RunCase(const Case& c, const contracts::VisibilityRule& rule, Schedule schedule,
                  std::uint64_t seed) {
    auto model = std::make_unique<Model>(c, rule, schedule, seed);
    return model->Run();
}

} // namespace harness
