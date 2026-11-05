// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_LIST_DIAG: the diagnostic build of docs/list-diag-design.md. Off by default. This header
// is what the emulator's hook sites call; the protocol itself is video_core/list_diag/protocol.h,
// shared with tools/list-diag-harness. T3a: the parts `pop` and `blk`. The part `list` is T3b and
// is only recognised here.
//
// Every wrapper starts with the read of the flag and a branch. With the diagnostic off nothing else
// runs, nothing is allocated (the state is created the first time a part is on) and no counter moves.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/hardware_properties.h"
#include "video_core/list_diag/protocol.h"

namespace VideoCommon::ListDiag {

enum Part : u32 {
    PartPop = 1,  ///< Problem A: the pop window
    PartBlk = 2,  ///< Problem B: 64-byte blocks
    PartList = 4, ///< Command list entries (T3b, not implemented yet)
};

/// EDEN_LIST_DIAG: absent, empty or "0": nothing. "1": every part. Otherwise a list separated by
/// commas of `pop`, `blk` and `list`.
inline u32 ParseParts(const char* value) {
    if (value == nullptr || value[0] == '\0' || (value[0] == '0' && value[1] == '\0')) {
        return 0;
    }
    if (value[0] == '1' && value[1] == '\0') {
        return PartPop | PartBlk | PartList;
    }
    u32 parts = 0;
    const char* p = value;
    while (*p != '\0') {
        const char* end = std::strchr(p, ',');
        const size_t length = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
        if (length == 3 && std::strncmp(p, "pop", 3) == 0) parts |= PartPop;
        if (length == 3 && std::strncmp(p, "blk", 3) == 0) parts |= PartBlk;
        if (length == 4 && std::strncmp(p, "list", 4) == 0) parts |= PartList;
        if (end == nullptr) break;
        p = end + 1;
    }
    return parts;
}

/// Read once at the start. Not changed during a session (only the tests change it, with the system stopped).
inline std::atomic<u32> g_parts{ParseParts(std::getenv("EDEN_LIST_DIAG"))};

[[nodiscard]] inline bool Enabled(u32 part) {
    return (g_parts.load(std::memory_order_relaxed) & part) != 0;
}
[[nodiscard]] inline bool NoticesEnabled() {
    return (g_parts.load(std::memory_order_relaxed) & (PartPop | PartBlk)) != 0;
}

using Impl = Protocol<Core::Hardware::NUM_CPU_CORES, ProdHooks>;

/// Where the log lines go: the emulator's log (list_diag.cpp), or the standard error without it.
using LogSink = void (*)(const std::string&);
inline std::atomic<LogSink> g_sink{nullptr};
void InstallLogSink(); ///< list_diag.cpp (the tests of the harness define their own)

inline void Emit(const std::string& line) {
    if (const LogSink sink = g_sink.load(std::memory_order_acquire)) {
        sink(line);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

inline std::atomic<Impl*> g_state{nullptr};

/// Allocated the first time a part is on, never otherwise.
inline Impl& State() {
    Impl* state = g_state.load(std::memory_order_acquire);
    if (state != nullptr) {
        return *state;
    }
    static Impl* const created = [] {
        InstallLogSink();
        auto* impl = new Impl();
        const u32 parts = g_parts.load(std::memory_order_relaxed);
        impl->pop_on = (parts & PartPop) != 0;
        impl->blk_on = (parts & PartBlk) != 0;
        g_state.store(impl, std::memory_order_release);
        return impl;
    }();
    return *created;
}

// ---------------------------------------------------------------------------------------------
// Hook sites.

/// core/memory.cpp, where the guest CPU holds a write: the Collect, observed when on.
template <class Manager>
inline void CollectNotice(Manager& manager, unsigned core, u64 address, size_t size) {
    if (!NoticesEnabled()) {
        manager.Collect(address, size);
        return;
    }
    State().CollectObserved(manager, core, address, static_cast<u32>(size));
}

/// buffer_cache.h, top of CachedWriteMemory.
template <class RangeSet, class ReadGuest>
inline void NoteApplyHook(u64 addr, u64 size, const RangeSet& gpu_modified, const RangeSet& cpu_read,
                          bool track_cpu_reads, ReadGuest&& read_guest) {
    if (!NoticesEnabled()) {
        return;
    }
    State().NoteApply(addr, size, gpu_modified, cpu_read, track_cpu_reads, read_guest);
}

/// buffer_cache.h, inside the loop of PopAsyncBuffers, before the bytes are written.
inline void NotePopHook(u64 start, u64 end, const u8* bytes) {
    if (!Enabled(PartPop)) {
        return;
    }
    State().NotePop(start, end, bytes);
}

inline std::string PartsText() {
    const u32 parts = g_parts.load(std::memory_order_relaxed);
    std::string text;
    for (const auto& [bit, name] : {std::pair<u32, const char*>{PartPop, "pop"}, {PartBlk, "blk"}, {PartList, "list"}}) {
        if ((parts & bit) != 0) {
            text += text.empty() ? name : std::string{","} + name;
        }
    }
    return text;
}

/// gpu.cpp, when the GPU starts: ONE line that says the diagnostic is on and which parts (nothing
/// at all when it is off). Without it a log with no [ListDiag] line cannot tell "off" from "broken".
inline void LogStartup() {
    if (g_parts.load(std::memory_order_relaxed) == 0) {
        return;
    }
    State(); // creates the state and installs the log sink
    Emit("[ListDiag] ligado: partes=" + PartsText() + (Enabled(PartList) ? " (list: T3b, ainda nao implementada)" : "") +
         "; resumo ao fechar o jogo e totais a cada 30 s");
}

/// gpu.cpp, after an application: the log, at most once per second, outside every cache lock; and
/// every 30 seconds a line of totals, so that the log holds data even if the game is killed
/// instead of closed from the menu (the summary is written at shutdown only).
inline void DrainLog() {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    Impl& state = State();
    state.MaybeDrain(Emit);
    static std::atomic<u64> last_heartbeat{0};
    const u64 now = ProdHooks::NowUs();
    u64 last = last_heartbeat.load(std::memory_order_relaxed);
    if (last == 0) {
        last_heartbeat.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last >= 30'000'000 && last_heartbeat.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        for (const std::string& line : state.SummaryLines("total")) {
            Emit(line);
        }
    }
}

/// back_thread_stats.cpp Report(): the lines of the periodic block, if the diagnostic is on.
inline std::vector<std::string> ReportLines(double elapsed_seconds) {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return {};
    }
    return State().ReportLines(elapsed_seconds);
}

/// At the end of the session: the summary (repeats the P0 notice if it applies).
inline void LogSummary() {
    if (g_state.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    Impl& state = State();
    state.Drain(Emit);
    for (const std::string& line : state.SummaryLines()) {
        Emit(line);
    }
}

} // namespace VideoCommon::ListDiag
