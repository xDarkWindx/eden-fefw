// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/back_thread_stats.h"

namespace VideoCommon::BackStats {

namespace {

int ReadMode() {
    const char* const value = std::getenv("EDEN_BACK_THREAD_STATS");
    if (value == nullptr || value[0] == '\0' || value[0] == '0') {
        return 0;
    }
    return value[0] == '2' ? 2 : 1;
}

u64 Take(std::atomic<u64>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point window_time{};
u64 window_stamp{};

constexpr std::array<const char*, DrawCount> DRAW_NAMES{
    "flush work",     "flush caching", "pipeline",      "lock wait",      "setup",
    "storage bind",   "handles",       "resolve",       "bind stage",     "buffers",
    "stage buffers",  "push images",   "post configure", "desc compare",  "desc allocate",
    "desc write",     "desc assign",   "desc record",   "dynamic states", "queries",
    "draw func",      "flush storage", "tail",          "unlock",         "tick work",
};

constexpr std::array<const char*, 9> DRAIN_NAMES{
    "other",   "bind",     "native acquire", "semaphore acquire", "acquire",
    "macro memory", "front 3D", "front compute", "signal sync",
};

} // Anonymous namespace

extern const int mode = ReadMode();
DrawThread draw_thread;
Shared shared;

void Report() {
    const auto now_time = std::chrono::steady_clock::now();
    const u64 now_stamp = Stamp();
    if (window_stamp == 0) {
        window_time = now_time;
        window_stamp = now_stamp;
        draw_thread = DrawThread{};
        return;
    }
    if (now_time - window_time < std::chrono::seconds{4}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now_time - window_time).count();
    const double wall_ticks = static_cast<double>(now_stamp - window_stamp);
    const double ticks_per_us = wall_ticks / (seconds * 1e6);
    window_time = now_time;
    window_stamp = now_stamp;

    DrawThread d = draw_thread;
    draw_thread = DrawThread{};
    draw_thread.last = d.last;
    draw_thread.report_check = d.report_check;

    // Percentages of the wall time (100% = one core busy), and per-second rates.
    const auto pct = [wall_ticks](u64 ticks) { return static_cast<double>(ticks) * 100.0 / wall_ticks; };
    const auto per_s = [seconds](u64 value) { return static_cast<double>(value) / seconds; };
    const auto us_per_s = [seconds, ticks_per_us](u64 ticks) {
        return static_cast<double>(ticks) / ticks_per_us / seconds;
    };
    const auto avg = [](u64 sum, u64 count) {
        return count != 0 ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
    };

    u64 marked{};
    std::string spans;
    for (u32 bucket = 0; bucket < DrawCount; ++bucket) {
        marked += d.draw[bucket];
        spans += fmt::format("{}{} {:.1f}%", bucket != 0 ? ", " : "", DRAW_NAMES[bucket],
                             pct(d.draw[bucket]));
    }
    const u64 busy = d.back_loop ? d.back_busy : 0;
    const u64 composite = Take(shared.composite);
    const u64 clear = Take(shared.clear);
    const u64 compute = Take(shared.compute);
    const u64 dma = Take(shared.dma);
    const u64 inline_upload = Take(shared.inline_upload);
    const u64 known = d.prepare + composite + clear + compute;
    LOG_INFO(Render_Vulkan,
             "Back thread: {:.0f} draws/s; busy {}; prepare draw {:.1f}% ({:.2f} us/draw, spans "
             "{:.1f}%), composite {:.1f}%, clear {:.1f}%, compute {:.1f}%, dma {:.1f}%, inline "
             "upload {:.1f}%, other busy {}; draw lock held {:.1f}%",
             per_s(d.draws),
             d.back_loop ? fmt::format("{:.1f}% (idle {:.1f}%)", pct(d.back_busy), pct(d.back_idle))
                         : std::string{"n/a (split off)"},
             pct(d.prepare), d.draws != 0 ? static_cast<double>(d.prepare) / ticks_per_us / static_cast<double>(d.draws) : 0.0,
             pct(marked), pct(composite), pct(clear), pct(compute), pct(dma),
             pct(inline_upload),
             d.back_loop ? fmt::format("{:.1f}%", busy > known ? pct(busy - known) : 0.0)
                         : std::string{"n/a"},
             pct(d.lock_hold));
    LOG_INFO(Render_Vulkan, "Back thread draw spans (% of wall time): {}", spans);

    const u64 paths = d.paths[PathBuffer] + d.paths[PathPush] + d.paths[PathSet] + d.paths[PathNone];
    LOG_INFO(Render_Vulkan,
             "Back thread descriptors: buffer {:.0f}/s, push {:.0f}/s, set {:.0f}/s, none {:.0f}/s "
             "(of {:.0f}/s); sets kept {:.0f}/s, rewritten for a pipeline bind {:.0f}/s; buffer "
             "allocations reused {:.1f}%, {:.0f} new/s with {:.0f} entries each; per draw {:.1f} KB copied, {:.1f} KB compared, {:.1f} KB assigned; ring "
             "exhausted {:.1f}/s, ring frame wait {:.0f} us/s",
             per_s(d.paths[PathBuffer]), per_s(d.paths[PathPush]), per_s(d.paths[PathSet]),
             per_s(d.paths[PathNone]), per_s(paths), per_s(d.set_reused), per_s(d.set_rebound),
             d.buffer_reused + d.buffer_allocated != 0
                 ? static_cast<double>(d.buffer_reused) * 100.0 /
                       static_cast<double>(d.buffer_reused + d.buffer_allocated)
                 : 0.0,
             per_s(d.buffer_allocated), avg(d.buffer_entries, d.buffer_allocated),
             avg(d.copied_bytes, d.draws) / 1024.0, avg(d.compared_bytes, d.draws) / 1024.0,
             avg(d.assigned_bytes, d.draws) / 1024.0, per_s(d.ring_exhausted),
             us_per_s(d.ring_frame_wait));

    const auto wait_text = [&](const char* name, WaitCounters& counters) {
        const u64 count = Take(counters.count);
        const u64 ticks = Take(counters.ticks);
        const u64 in_lock = Take(counters.ticks_in_draw_lock);
        const u64 queue_sum = Take(counters.queue_sum);
        const u64 queue_max = Take(counters.queue_max);
        const u64 retained = Take(counters.retained);
        return fmt::format("{} {:.1f}/s {:.0f} us/s (in draw lock {:.0f} us/s, queue avg {:.1f} max "
                           "{}, producer retained {:.0f}%)",
                           name, per_s(count), us_per_s(ticks), us_per_s(in_lock),
                           avg(queue_sum, count), queue_max,
                           count != 0 ? static_cast<double>(retained) * 100.0 /
                                            static_cast<double>(count)
                                      : 0.0);
    };
    const std::string finish = wait_text("Finish", shared.finish);
    const std::string wait_worker = wait_text("WaitWorker", shared.wait_worker);
    const std::string wait_tick = wait_text("Wait(tick, also inside Finish)", shared.wait_tick);
    const std::string wait_tick_other = wait_text("Wait(tick) on other threads", shared.wait_tick_other);
    LOG_INFO(Render_Vulkan, "Back thread waits: {}; {}; {}; {}", finish, wait_worker, wait_tick,
             wait_tick_other);

    const u64 worker_busy = Take(shared.worker_busy);
    const u64 worker_idle = Take(shared.worker_idle);
    const u64 chunks = Take(shared.worker_chunks);
    const u64 age_max = Take(shared.worker_age_max);
    const u64 submits = Take(shared.submits);
    const u64 submit_latency = Take(shared.submit_latency);
    const u64 submit_latency_max = Take(shared.submit_latency_max);
    const u64 dispatches = Take(shared.dispatches);
    const u64 dispatch_queue_sum = Take(shared.dispatch_queue_sum);
    const u64 dispatch_queue_max = Take(shared.dispatch_queue_max);
    LOG_INFO(Render_Vulkan,
             "Vulkan worker: busy {:.1f}% (idle {:.1f}%), {:.0f} chunks/s, queue at dispatch avg "
             "{:.2f} max {}, oldest command age max {} frames; {:.0f} submits/s, record to submit "
             "avg {:.0f} us max {:.0f} us",
             pct(worker_busy), pct(worker_idle), per_s(chunks), avg(dispatch_queue_sum, dispatches),
             dispatch_queue_max, age_max, per_s(submits),
             submits != 0 ? static_cast<double>(submit_latency) / ticks_per_us /
                                static_cast<double>(submits)
                          : 0.0,
             static_cast<double>(submit_latency_max) / ticks_per_us);

    const auto lock_text = [&](const char* name, LockCounters& counters) {
        const u64 count = Take(counters.count);
        const u64 ticks = Take(counters.ticks);
        return fmt::format("{} {:.0f}/s waiting {:.0f} us/s", name, per_s(count), us_per_s(ticks));
    };
    const std::string cpu_write = lock_text("OnCPUWrite", shared.cpu_write);
    const std::string cached_writes = lock_text("cached writes", shared.cached_writes);
    const std::string other_locks = lock_text("other", shared.other_locks);
    const std::string draw_thread_locks = lock_text("draw thread outside draws", shared.draw_thread_locks);
    std::string drain_sites;
    for (size_t site = 0; site < DRAIN_NAMES.size(); ++site) {
        const u64 count = Take(shared.drain_sites[site].count);
        const u64 ticks = Take(shared.drain_sites[site].ticks);
        if (count != 0) {
            drain_sites += fmt::format("{}{} {:.0f}/s {:.0f} us/s", drain_sites.empty() ? "" : ", ",
                                       DRAIN_NAMES[site], per_s(count), us_per_s(ticks));
        }
    }
    const u64 acquire_ready = Take(shared.acquire_ready);
    const u64 acquire_after_fences = Take(shared.acquire_ready_after_fences);
    const u64 acquire_suspended = Take(shared.acquire_suspended);
    const u64 acquire_fence_ticks = Take(shared.acquire_fence_ticks);
    LOG_INFO(Render_Vulkan,
             "Split drains by caller (count/s, front waiting): {}. Native acquires: ready {:.0f}/s, "
             "ready after releasing fences {:.0f}/s, suspended {:.0f}/s; releasing fences {:.0f} us/s",
             drain_sites.empty() ? std::string{"none"} : drain_sites, per_s(acquire_ready),
             per_s(acquire_after_fences), per_s(acquire_suspended), us_per_s(acquire_fence_ticks));
    const u64 drains = Take(shared.drains);
    const u64 drain_ticks = Take(shared.drain_ticks);
    const u64 space_ticks = Take(shared.space_ticks);
    LOG_INFO(Render_Vulkan,
             "Cache locks: {}; {}; {}; {}. Split front: {:.0f} drains/s waiting {:.0f} us/s, ring "
             "space {:.0f} us/s",
             cpu_write, cached_writes, other_locks, draw_thread_locks, per_s(drains),
             us_per_s(drain_ticks), us_per_s(space_ticks));
}

} // namespace VideoCommon::BackStats
