// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

#if defined(ARCHITECTURE_x86_64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#include "common/common_types.h"

// Where the thread that prepares draws (the GPU back thread with the split, the GPU thread
// without it) spends its time, and what it waits for (docs/back-thread-split-plan.md, phase 0).
// EDEN_BACK_THREAD_STATS=1 logs it every 4 s; =2 also serializes the stamps (lfence before rdtsc),
// to check the buckets a decision rests on. Off, every probe is one load and a branch.
//
// Ownership: DrawThread is touched only by the thread that prepares draws (one at a time: the
// split hands it over with a join or a drain). Everything other threads write is in Shared, as
// relaxed atomics: each value is atomic by itself and a report a chunk late does not matter.
namespace VideoCommon::BackStats {

/// 0 off, 1 on, 2 on with serialized stamps.
extern const int mode;

[[nodiscard]] inline bool Enabled() {
    return mode != 0;
}

/// A time stamp in ticks (the TSC on x86-64, steady_clock elsewhere); the report calibrates them.
/// Unserialized rdtsc may move by tens of cycles: meaningful for buckets summed over thousands of
/// draws, not for a single short span.
[[nodiscard]] inline u64 Stamp() {
#if defined(ARCHITECTURE_x86_64)
    if (mode == 2) {
        _mm_lfence();
    }
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/// Consecutive spans of RasterizerVulkan::PrepareDraw, each attributed by Mark to one bucket.
enum Draw : u32 {
    FlushWork,     ///< FlushWork, FlushCaching
    Pipeline,      ///< pipeline_cache.CurrentGraphicsPipeline
    LockWait,      ///< Acquiring buffer_cache.mutex + texture_cache.mutex
    Handles,       ///< ConfigureImpl: VPRS, storage buffers, texture handles, compact tables
    Resolve,       ///< ConfigureImpl: view and sampler resolution, banks
    BindStage,     ///< ConfigureImpl: texture and image buffers
    Buffers,       ///< UpdateGraphicsBuffers, BindHostGeometryBuffers, descriptor queue Acquire
    StageBuffers,  ///< BindHostStageBuffers
    PushImages,    ///< PushImageDescriptors
    PostConfigure, ///< PostCopyBarrier, UpdateRenderTargets, CheckFeedbackLoop
    DescCompare,   ///< ConfigureDraw: comparing the payload with the last one
    DescAllocate,  ///< DescriptorBufferRing::Allocate
    DescWrite,     ///< WriteDescriptorBuffer
    DescAssign,    ///< Keeping the payload (last_descriptor_payload.assign)
    DescRecord,    ///< Rest of ConfigureDraw: render pass, pipeline and state updates, Record
    DynamicStates, ///< UpdateDynamicStates
    Queries,       ///< Query segment, transform feedback, counters
    DrawFunc,      ///< Recording the draw
    FlushStorage,  ///< FlushStorageWrites
    Tail,          ///< Unlock, early exits, failed configures
    TickWork,      ///< gpu.TickWork at the end
    DrawCount,
};

/// The draw-preparing thread's own counters (see the ownership note above).
struct DrawThread {
    u64 last{};
    std::array<u64, DrawCount> draw{};
    u64 draws{};
    u64 prepare{};   ///< Whole PrepareDraw calls
    u64 lock_hold{}; ///< From acquiring the cache locks to releasing them
    u64 back_busy{}; ///< GpuSplit::BackLoop executing entries
    u64 back_idle{}; ///< ... spinning or sleeping
    bool back_loop{}; ///< BackLoop ran in this window
    // Descriptors of graphics draws.
    std::array<u64, 4> paths{}; ///< Descriptor buffer, push descriptor, descriptor set, none
    u64 buffer_reused{};
    u64 buffer_allocated{};
    u64 buffer_entries{};       ///< Entries written into new allocations (one vkGetDescriptorEXT each)
    u64 copied_bytes{};         ///< Cached compact-table entries copied (AddEntries)
    u64 compared_bytes{};
    u64 assigned_bytes{};
    u64 ring_exhausted{};       ///< Descriptor buffer frames that ran out (a Finish each)
    u64 ring_frame_wait{};      ///< Waiting for a reused descriptor buffer frame
    u32 report_check{};
};
extern DrawThread draw_thread;

enum DescriptorPath : u32 {
    PathBuffer,
    PathPush,
    PathSet,
    PathNone,
};

/// A scheduler wait (Finish, WaitWorker, Wait for a tick).
struct WaitCounters {
    std::atomic<u64> count{};
    std::atomic<u64> ticks{};
    std::atomic<u64> ticks_in_draw_lock{}; ///< While the draw thread held the cache locks
    std::atomic<u64> queue_sum{};          ///< Chunks queued for the worker when the wait began
    std::atomic<u64> queue_max{};
    std::atomic<u64> retained{};           ///< The producer's chunk still held commands
};

struct LockCounters {
    std::atomic<u64> count{};
    std::atomic<u64> ticks{};
};

/// Written by other threads (worker, guest CPU threads, the split's front).
struct Shared {
    std::atomic<u64> frame{}; ///< TickFrame count of the draw thread
    std::atomic<u64> composite{}; ///< RendererVulkan::Composite (may run on a syncpoint action)
    std::atomic<u64> clear{};
    std::atomic<u64> compute{};
    std::atomic<u64> dma{};
    std::atomic<u64> inline_upload{};
    // Vulkan worker.
    std::atomic<u64> worker_busy{};
    std::atomic<u64> worker_idle{};
    std::atomic<u64> worker_chunks{};
    std::atomic<u64> worker_age_max{}; ///< Frames since the oldest command of a chunk was recorded
    std::atomic<u64> submits{};
    std::atomic<u64> submit_latency{}; ///< From recording a submission to running it
    std::atomic<u64> submit_latency_max{};
    std::atomic<u64> dispatches{};
    std::atomic<u64> dispatch_queue_sum{};
    std::atomic<u64> dispatch_queue_max{};
    WaitCounters finish;
    WaitCounters wait_worker;
    WaitCounters wait_tick;
    // Cache locks taken outside draws: by other threads, and by the draw thread itself.
    LockCounters cpu_write;
    LockCounters cached_writes;
    LockCounters other_locks;
    LockCounters draw_thread_locks;
    // The split's front.
    std::atomic<u64> drains{};
    std::atomic<u64> drain_ticks{};
    std::atomic<u64> space_ticks{};
};
extern Shared shared;

/// Whether this thread prepares draws (set by PrepareDraw).
inline thread_local bool is_draw_thread{};
/// Whether this thread holds the cache locks for a draw.
inline thread_local bool in_draw_lock{};

inline void Add(std::atomic<u64>& counter, u64 value) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

inline void Max(std::atomic<u64>& counter, u64 value) {
    u64 current = counter.load(std::memory_order_relaxed);
    while (value > current &&
           !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

/// Starts the chain of draw spans.
inline void Begin() {
    if (Enabled()) {
        draw_thread.last = Stamp();
    }
}

/// Ends the current draw span in `bucket` and starts the next.
inline void Mark(Draw bucket) {
    if (Enabled()) {
        const u64 now = Stamp();
        draw_thread.draw[bucket] += now - draw_thread.last;
        draw_thread.last = now;
    }
}

/// Adds the time of a scope to a draw-thread counter.
class ScopedTime {
public:
    explicit ScopedTime(u64& counter_) : counter{counter_}, start{Enabled() ? Stamp() : 0} {}
    ~ScopedTime() {
        if (start != 0) {
            counter += Stamp() - start;
        }
    }
    ScopedTime(const ScopedTime&) = delete;
    ScopedTime& operator=(const ScopedTime&) = delete;

private:
    u64& counter;
    u64 start;
};

/// Adds the time of a scope to a shared counter (any thread).
class ScopedSharedTime {
public:
    explicit ScopedSharedTime(std::atomic<u64>& counter_)
        : counter{counter_}, start{Enabled() ? Stamp() : 0} {}
    ~ScopedSharedTime() {
        if (start != 0) {
            Add(counter, Stamp() - start);
        }
    }
    ScopedSharedTime(const ScopedSharedTime&) = delete;
    ScopedSharedTime& operator=(const ScopedSharedTime&) = delete;

private:
    std::atomic<u64>& counter;
    u64 start;
};

/// Times a cache lock acquisition outside draws: construct before the lock, call Acquired after it.
class LockTimer {
public:
    explicit LockTimer(LockCounters& counters_)
        : counters{is_draw_thread ? shared.draw_thread_locks : counters_},
          start{Enabled() ? Stamp() : 0} {}
    void Acquired() const {
        if (start != 0) {
            Add(counters.count, 1);
            Add(counters.ticks, Stamp() - start);
        }
    }

private:
    LockCounters& counters;
    u64 start;
};

/// Constructed right after a draw takes the cache locks, destroyed right before it releases them.
class DrawLockHold {
public:
    DrawLockHold() : start{Enabled() ? Stamp() : 0} {
        in_draw_lock = true;
    }
    ~DrawLockHold() {
        in_draw_lock = false;
        if (start != 0) {
            draw_thread.lock_hold += Stamp() - start;
        }
    }
    DrawLockHold(const DrawLockHold&) = delete;
    DrawLockHold& operator=(const DrawLockHold&) = delete;

private:
    u64 start;
};

/// A scheduler wait: construct with the queue state when it begins, Done when it ends.
class WaitTimer {
public:
    WaitTimer(WaitCounters& counters_, u64 queued, bool retained)
        : counters{counters_}, start{Enabled() ? Stamp() : 0} {
        if (start != 0) {
            Add(counters.count, 1);
            Add(counters.queue_sum, queued);
            Max(counters.queue_max, queued);
            if (retained) {
                Add(counters.retained, 1);
            }
        }
    }
    ~WaitTimer() {
        if (start != 0) {
            const u64 ticks = Stamp() - start;
            Add(counters.ticks, ticks);
            if (in_draw_lock) {
                Add(counters.ticks_in_draw_lock, ticks);
            }
        }
    }
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;

private:
    WaitCounters& counters;
    u64 start;
};

/// Logs and resets the counters every 4 s (draw thread only).
void Report();

/// Calls Report every so many draws (draw thread only).
inline void MaybeReport() {
    if (Enabled() && (++draw_thread.report_check & 255) == 0) {
        Report();
    }
}

} // namespace VideoCommon::BackStats
