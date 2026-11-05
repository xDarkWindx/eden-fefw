// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "model.h"

#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <random>
#include <set>
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

const char* HostModeName(HostMode m) {
    return m == HostMode::Early ? "early" : "deferred";
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

enum class RingKind {
    None,
    Produce,
    Marker,
    SyncptIncr,
    SemRelease,
    Copy,
    FetchMarker,
    NoteRead, // the guest CPU read these words (they are worth downloading)
    HeldApply, // InvalidateGPUCache: apply the held notices
    Fence,    // a fence: queue the download of the GPU written, CPU read words
    Stop
};

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

enum class GpuKind { None, Write, Copy, SyncptIncr, SemRelease, Download, Stop };

// One queued download: the words copied GPU -> staging when the fence's copy executes, and
// applied to guest memory when the fence completes. `live` says which words are still wanted:
// a CPU write over a word cancels it.
struct Download {
    std::vector<std::uint32_t> words; // sorted, unique
    std::vector<Word> staged;
    std::vector<std::uint8_t> live;
};

struct GpuCmd {
    GpuKind kind = GpuKind::None;
    Seq seq = 0;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::vector<std::uint32_t> phys; // Write: one address per value
    std::vector<Word> values;
    std::shared_ptr<Transfer> transfer;
    std::shared_ptr<Download> download;
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
    OpKind parked_kind = OpKind::Wfi; // the acquire the channel is parked on, when it is
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

class ModelDirect final : public contracts::DirectHostRead {
public:
    explicit ModelDirect(Model& m) : m_{m} {}
    std::vector<Word> ReadHostDirect(const std::vector<std::uint32_t>& phys) override;

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
          HostMode host_mode, std::uint64_t seed)
        : case_{c}, rule_{rule}, schedule_{schedule}, host_mode_{host_mode}, knobs_{c.knobs},
          seed_{seed}, front_jitter_{seed * 3 + 1}, memory_{*this}, back_progress_{*this},
          direct_{*this},
          consumer_{rule, memory_, back_progress_, c.knobs.honor_valid_until,
                    c.knobs.trust_syncpoint_as_ready ? &direct_ : nullptr} {
        guest_.resize(kWords);
        host_.resize(kWords);
        cpu_dirty_.assign(kWords, 0);
        gpu_modified_.assign(kWords, 0);
        cpu_read_.assign(kWords, 0);
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

    std::vector<Word> ReadHostDirect(const std::vector<std::uint32_t>& phys) {
        std::lock_guard lock{host_mutex_};
        std::vector<Word> out(phys.size());
        for (std::size_t i = 0; i < phys.size(); ++i) {
            out[i] = host_[phys[i]];
        }
        return out;
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
            // IncrementGuest). The host value (IncrementHost) follows when the back thread
            // records the increment (host-early) or when the GPU executes it (host-deferred).
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
                if (op.kind == OpKind::SyncptAcquire && host_mode_ == HostMode::Early &&
                    gpu_executed_.load(std::memory_order_acquire) <
                        gpu_enqueued_.load(std::memory_order_acquire)) {
                    // The syncpoint is satisfied while the GPU has not executed what the
                    // back thread recorded: order is proven, completion is not.
                    result_.syncpoint_ahead_of_gpu = true;
                }
                if (ch.suspended) {
                    result_.suspend_diagnostic += " [resumed later: " + why + "]";
                    ch.suspended = false;
                }
                ch.waiting = false;
                PassDependency(ch, op);
                return true;
            }
            ch.parked_kind = op.kind;
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
                std::lock_guard dl{dl_mutex_};
                for (std::size_t i = 0; i < phys.size(); ++i) {
                    guest_[phys[i]] = op.values[i];
                    if (held.entries.empty()) {
                        cpu_dirty_[phys[i]] = 1;
                        // The write supersedes what the GPU wrote there (WriteMemory subtracts
                        // the written range from gpu_modified_ranges); a held write does it
                        // when it is applied, here when it is uploaded.
                        gpu_modified_[phys[i]] = 0;
                    }
                }
                if (knobs_.cancel_pending_downloads) {
                    CancelPendingDownloadsLocked(phys);
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
        case OpKind::CpuRead: {
            RingItem item;
            item.kind = RingKind::NoteRead;
            std::vector<std::uint32_t> phys(op.len);
            for (std::uint32_t i = 0; i < op.len; ++i) {
                phys[i] = op.pa + i;
            }
            item.ranges = Coalesce(phys);
            Push(std::move(item));
            return true;
        }
        case OpKind::Fence: {
            RingItem item;
            item.kind = RingKind::Fence;
            Push(std::move(item));
            return true;
        }
        case OpKind::WaitBack:
            WaitBackPassed(tail_seq_);
            return true;
        case OpKind::CpuWriteHeld:
        case OpKind::CpuWriteHeldNotice:
        case OpKind::CpuWriteHeldBytes: {
            // The combined op does (i-a), (i-b) and (i-c) in a row. The split ops let a case put
            // another step (the application of the notice) between the notice and the memcpy:
            // the emulator registers the notice inside GetPointerImpl and the caller does the
            // memcpy afterwards (core/memory.cpp:587-590, 649-653).
            const bool do_notice = op.kind != OpKind::CpuWriteHeldBytes;
            const bool do_bytes = op.kind != OpKind::CpuWriteHeldNotice;
            if (!do_notice) {
                // Only the bytes: nothing to do unless the notice op held this write.
                if (split_pending_.erase(op.pa) == 0) {
                    return true;
                }
            }
            std::vector<std::uint32_t> phys(op.values.size());
            for (std::size_t i = 0; i < phys.size(); ++i) {
                phys[i] = op.pa + static_cast<std::uint32_t>(i);
            }
            // Critical section 1: OnCPUWrite under the buffer cache mutex decides whether the
            // page holds GPU data. Nothing else is done there for a held write: no
            // ClearDownload, no gpu_modified_ranges change (buffer_cache.h:169-174).
            bool held = !do_notice; // the bytes op only runs for a write that was held
            if (do_notice) {
                std::lock_guard dl{dl_mutex_};
                for (std::uint32_t w : phys) {
                    const std::uint32_t page_begin = w / kPageWords * kPageWords;
                    for (std::uint32_t a = page_begin; a < page_begin + kPageWords && !held; ++a) {
                        held = gpu_modified_[a] != 0;
                    }
                }
            }
            if (!held) {
                // Not held: WriteMemory at once, as CpuWrite does.
                std::lock_guard lock{guest_mutex_};
                std::lock_guard dl{dl_mutex_};
                for (std::size_t i = 0; i < phys.size(); ++i) {
                    guest_[phys[i]] = op.values[i];
                    cpu_dirty_[phys[i]] = 1;
                    gpu_modified_[phys[i]] = 0;
                }
                if (knobs_.cancel_pending_downloads) {
                    CancelPendingDownloadsLocked(phys);
                }
                return true;
            }
            // Critical section 2: GPUDirtyMemoryManager::Collect (core/memory.cpp:765). The
            // notice is the written words, or the 64-byte blocks that contain them
            // (notice_block_words): the bytes written to guest memory below stay the exact ones.
            if (do_notice) {
                std::vector<std::uint32_t> notice = phys;
                if (knobs_.notice_block_words != 0) {
                    const std::uint32_t block = knobs_.notice_block_words;
                    const std::uint32_t begin = phys.front() / block * block;
                    const std::uint32_t end = (phys.back() / block + 1) * block;
                    notice.clear();
                    for (std::uint32_t w = begin; w < end; ++w) {
                        notice.push_back(w);
                    }
                }
                std::lock_guard lock{held_mutex_};
                held_notices_.push_back(std::move(notice));
            }
            if (!do_bytes) {
                split_pending_.insert(op.pa); // the memcpy comes with a later CpuWriteHeldBytes
                return true;
            }
            // Critical section 3: the memcpy into guest memory, outside every cache lock.
            {
                std::lock_guard lock{guest_mutex_};
                for (std::size_t i = 0; i < phys.size(); ++i) {
                    guest_[phys[i]] = op.values[i];
                }
            }
            for (std::uint32_t page = op.pa / kPageWords;
                 page <= (op.pa + static_cast<std::uint32_t>(phys.size()) - 1) / kPageWords;
                 ++page) {
                page_epoch_[page].fetch_add(1, std::memory_order_acq_rel);
            }
            return true;
        }
        case OpKind::ApplyHeld: {
            RingItem item;
            item.kind = RingKind::HeldApply;
            Push(std::move(item));
            return true;
        }
        case OpKind::WaitDownloads:
            WaitBackPassed(tail_seq_);
            while (downloads_applied_.load(std::memory_order_acquire) <
                       downloads_queued_.load(std::memory_order_acquire) &&
                   !Hung()) {
                RaiseGpu(tail_seq_);
                std::this_thread::yield();
            }
            return true;
        case OpKind::CheckGuest: {
            std::lock_guard lock{guest_mutex_};
            for (std::size_t i = 0; i < op.expected.size(); ++i) {
                if (guest_[op.pa + i] != op.expected[i]) {
                    std::ostringstream s;
                    s << "guest memory word " << (op.pa + i) << ": got 0x" << std::hex
                      << guest_[op.pa + i] << ", expected 0x" << op.expected[i] << std::dec;
                    Fail(FailureKind::WordMismatch, s.str());
                    break;
                }
            }
            return true;
        }
        case OpKind::CheckHost: {
            WaitBackPassed(tail_seq_);
            while (gpu_executed_.load(std::memory_order_acquire) <
                       gpu_enqueued_.load(std::memory_order_acquire) &&
                   !Hung()) {
                RaiseGpu(tail_seq_);
                std::this_thread::yield();
            }
            std::lock_guard lock{host_mutex_};
            for (std::size_t i = 0; i < op.expected.size(); ++i) {
                if (host_[op.pa + i] != op.expected[i]) {
                    std::ostringstream s;
                    s << "host buffer word " << (op.pa + i) << ": got 0x" << std::hex
                      << host_[op.pa + i] << ", expected 0x" << op.expected[i] << std::dec;
                    Fail(FailureKind::WordMismatch, s.str());
                    break;
                }
            }
            return true;
        }
        }
        return true;
    }

    bool CoveredByHeldNotice(std::uint32_t w) {
        std::lock_guard lock{held_mutex_};
        for (const auto& notice : held_notices_) {
            if (std::find(notice.begin(), notice.end(), w) != notice.end()) {
                return true;
            }
        }
        return false;
    }

    // A CPU write over a word cancels the pending downloads of it: the old GPU data must not
    // land over the new bytes (ClearDownload, buffer_cache.h:209-216). Caller holds guest_mutex_
    // and dl_mutex_.
    void CancelPendingDownloadsLocked(const std::vector<std::uint32_t>& phys) {
        for (auto& d : pending_downloads_) {
            for (std::uint32_t w : phys) {
                const auto it = std::lower_bound(d->words.begin(), d->words.end(), w);
                if (it != d->words.end() && *it == w) {
                    d->live[static_cast<std::size_t>(it - d->words.begin())] = 0;
                }
            }
        }
        if (knobs_.single_pending_set) {
            for (std::uint32_t w : phys) {
                shared_pending_.erase(w);
            }
        }
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
                // The GPU is only let run when something parked can depend on it: a semaphore
                // (the GPU releases it), a syncpoint in host-deferred mode (the GPU raises
                // the host value), or when everything left is suspended. In host-early mode a
                // syncpoint needs only the back thread, so the GPU keeps lagging.
                bool gpu_needed = all_remaining_suspended;
                for (const ChannelState& parked : channels_) {
                    if (!parked.done && (parked.parked_kind == OpKind::SemAcquire ||
                                         (parked.parked_kind == OpKind::SyncptAcquire &&
                                          host_mode_ == HostMode::Deferred))) {
                        gpu_needed = true;
                    }
                }
                RaiseBack(tail_seq_);
                if (gpu_needed) {
                    RaiseGpu(tail_seq_);
                }
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

    // InvalidateGPUCache -> OnCachedWrites -> CachedWriteMemory (buffer_cache.h:141-157): for each
    // held notice, read the words from guest memory NOW and upload them to the host buffer, after
    // dropping the pending downloads and the GPU ranges over them (InlineMemoryImplementation,
    // :2107-2108). One critical section of its own: not held while the write was made nor while
    // the downloads were popped.
    void ApplyHeldNotices(Seq seq) {
        GpuCmd cmd;
        cmd.kind = GpuKind::Write;
        cmd.seq = seq;
        {
            std::lock_guard lock{guest_mutex_};
            std::lock_guard dl{dl_mutex_};
            std::lock_guard held{held_mutex_};
            for (const auto& notice : held_notices_) {
                for (std::uint32_t w : notice) {
                    cmd.phys.push_back(w);
                    cmd.values.push_back(guest_[w]);
                    gpu_modified_[w] = 0;
                    cpu_dirty_[w] = 0;
                }
                if (knobs_.cancel_pending_downloads) {
                    CancelPendingDownloadsLocked(notice);
                }
            }
            held_notices_.clear();
        }
        // Pushed with no lock held: the GPU thread takes the same locks to apply downloads.
        if (!cmd.phys.empty()) {
            PushGpu(std::move(cmd));
        }
    }

    // Records the upload of CPU-written words in `ranges`, as the buffer cache does
    // before a use. Exact words by default; whole pages with the negative knob.
    void UploadCpuWrites(Seq seq, const std::vector<PhysRange>& ranges) {
        GpuCmd cmd;
        cmd.kind = GpuKind::Write;
        cmd.seq = seq;
        {
            std::lock_guard lock{guest_mutex_};
            std::lock_guard dl{dl_mutex_};
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
        }
        // Pushed with no lock held: the GPU thread takes both to apply downloads.
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
                {
                    std::lock_guard dl{dl_mutex_};
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
                if (host_mode_ == HostMode::Early) {
                    // The SignalFence callback runs when the fence is signalled, that is, now
                    // that the back thread records the increment: before the GPU gets there.
                    host_syncpoint_[item->index].fetch_add(1, std::memory_order_acq_rel);
                    break;
                }
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
                std::unique_lock<std::mutex> dl{dl_mutex_};
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
                dl.unlock();
                GpuCmd cmd;
                cmd.kind = GpuKind::Copy;
                cmd.seq = seq;
                cmd.transfer = item->transfer;
                PushGpu(std::move(cmd));
                break;
            }
            case RingKind::HeldApply:
                ApplyHeldNotices(seq);
                break;
            case RingKind::NoteRead: {
                std::lock_guard dl{dl_mutex_};
                for (const PhysRange& r : item->ranges) {
                    for (std::uint32_t pa = r.begin; pa < r.end; ++pa) {
                        cpu_read_[pa] = 1;
                    }
                }
                break;
            }
            case RingKind::Fence: {
                // Download at the fence only the words the GPU wrote and the CPU reads
                // (buffer_cache_base.h:552-562 for Switch 2 channels).
                std::shared_ptr<Download> download;
                {
                    std::lock_guard dl{dl_mutex_};
                    std::vector<std::uint32_t> words;
                    for (std::uint32_t pa = 0; pa < kWords; ++pa) {
                        if (gpu_modified_[pa] && cpu_read_[pa]) {
                            words.push_back(pa);
                        }
                    }
                    if (!words.empty()) {
                        download = std::make_shared<Download>();
                        download->staged.assign(words.size(), 0);
                        download->live.assign(words.size(), 1);
                        if (knobs_.single_pending_set) {
                            shared_pending_.insert(words.begin(), words.end());
                        }
                        download->words = std::move(words);
                        pending_downloads_.push_back(download);
                        downloads_queued_.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                if (download) {
                    GpuCmd cmd;
                    cmd.kind = GpuKind::Download;
                    cmd.seq = seq;
                    cmd.download = std::move(download);
                    PushGpu(std::move(cmd));
                }
                // SignalFence ends with InvalidateGPUCache (fence_manager.h:139).
                ApplyHeldNotices(seq);
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
            case GpuKind::Write: {
                std::lock_guard lock{host_mutex_};
                for (std::size_t i = 0; i < cmd->phys.size(); ++i) {
                    host_[cmd->phys[i]] = cmd->values[i];
                }
                break;
            }
            case GpuKind::Copy: {
                Transfer& t = *cmd->transfer;
                {
                    std::lock_guard lock{host_mutex_};
                    for (const PhysRange& r : t.ranges) {
                        t.words.emplace_back(host_.begin() + r.begin, host_.begin() + r.end);
                    }
                }
                t.completion_fence->signaled.store(true, std::memory_order_release);
                break;
            }
            case GpuKind::Download: {
                Download& d = *cmd->download;
                {
                    // GPU -> staging, ordered in the stream after every earlier write.
                    std::lock_guard lock{host_mutex_};
                    for (std::size_t i = 0; i < d.words.size(); ++i) {
                        d.staged[i] = host_[d.words[i]];
                    }
                }
                // The fence completes some time after the copy; the CPU may write meanwhile.
                if (schedule_ == Schedule::Random) {
                    jitter.Maybe();
                }
                {
                    // The pop (PopAsyncBuffers, buffer_cache.h:703-730) is its own critical
                    // section on the fence thread, under the buffer cache mutex.
                    std::lock_guard lock{guest_mutex_};
                    std::lock_guard dl{dl_mutex_};
                    for (std::size_t i = 0; i < d.words.size(); ++i) {
                        // Single set (before 15126fb): whichever download completes first
                        // consumes the pending entry of the word, old or new.
                        const bool live = knobs_.single_pending_set
                                              ? shared_pending_.erase(d.words[i]) > 0
                                              : d.live[i] != 0;
                        if (live && knobs_.pop_skips_held && CoveredByHeldNotice(d.words[i])) {
                            continue; // the held write's bytes are newer than the staged copy
                        }
                        if (live) {
                            guest_[d.words[i]] = d.staged[i];
                        }
                    }
                    pending_downloads_.erase(std::remove_if(pending_downloads_.begin(),
                                                            pending_downloads_.end(),
                                                            [&](const auto& p) {
                                                                return p.get() == &d;
                                                            }),
                                             pending_downloads_.end());
                }
                downloads_applied_.fetch_add(1, std::memory_order_acq_rel);
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
    const HostMode host_mode_;
    const ModelKnobs knobs_;
    const std::uint64_t seed_;
    Jitter front_jitter_;
    ModelMemory memory_;
    ModelBack back_progress_;
    ModelDirect direct_;
    contracts::ListFetchConsumer consumer_;
    Clock::time_point deadline_;

    // Guest memory and the CPU-written marks, shared by the front (CPU, snapshots) and the
    // back thread (uploads).
    std::mutex guest_mutex_;
    std::vector<Word> guest_;
    std::vector<std::uint8_t> cpu_dirty_;

    // The host buffer: written by the GPU thread, read by it and, for the negative control and
    // the final check, by the front under the same lock.
    std::mutex host_mutex_;
    std::vector<Word> host_;

    // The owner's exact ranges and the downloads, shared by the back thread (records, queues),
    // the GPU thread (applies) and the front (CPU writes cancel): the buffer cache's mutex.
    // Lock order: guest_mutex_, then dl_mutex_, then held_mutex_; host_mutex_ is a leaf. No lock
    // is ever held across the three steps of a held write (see CpuWriteHeld).
    std::mutex dl_mutex_;
    std::vector<std::uint8_t> gpu_modified_;
    std::vector<std::uint8_t> cpu_read_;
    std::vector<std::shared_ptr<Download>> pending_downloads_;
    std::set<std::uint32_t> shared_pending_;
    // Held notices (GPUDirtyMemoryManager): registered by the front, applied by the back thread.
    // A leaf-most lock: order guest_mutex_, dl_mutex_, then held_mutex_. The front takes it alone.
    std::mutex held_mutex_;
    std::vector<std::vector<std::uint32_t>> held_notices_;
    std::set<std::uint32_t> split_pending_; // held writes whose memcpy is a separate op (front only)
    std::atomic<std::uint64_t> downloads_queued_{0};
    std::atomic<std::uint64_t> downloads_applied_{0};

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
std::vector<Word> ModelDirect::ReadHostDirect(const std::vector<std::uint32_t>& phys) {
    return m_.ReadHostDirect(phys);
}

} // namespace

RunResult RunCase(const Case& c, const contracts::VisibilityRule& rule, Schedule schedule,
                  HostMode host_mode, std::uint64_t seed) {
    auto model = std::make_unique<Model>(c, rule, schedule, host_mode, seed);
    return model->Run();
}

} // namespace harness
