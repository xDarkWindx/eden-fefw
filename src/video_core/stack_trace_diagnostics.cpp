// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/stack_trace_diagnostics.h"

#include <atomic>
#include <cstdlib>

#include "common/logging.h"

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#undef near
#undef far

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace VideoCommon {

namespace {
std::atomic<u64> watched_addr{};
std::atomic<u64> watched_size{};
} // Anonymous namespace

bool StackTraceEnabled() {
#ifdef _MSC_VER
    static const bool enabled = std::getenv("EDEN_FINISH_TRACE") != nullptr;
    return enabled;
#else
    return false;
#endif
}

void WatchStackTraceRange(u64 device_addr, u64 size) {
    if (watched_addr.load(std::memory_order_relaxed) == device_addr &&
        watched_size.load(std::memory_order_relaxed) == size) {
        return;
    }
    watched_addr.store(device_addr, std::memory_order_relaxed);
    watched_size.store(size, std::memory_order_relaxed);
    static std::atomic<u32> lines{};
    if (lines.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_ERROR(Render_Vulkan, "StackTrace watching GPU-written range {:#x}+{:#x}", device_addr,
                  size);
    }
}

bool InWatchedStackTraceRange(u64 device_addr, u64 size) {
    const u64 begin = watched_addr.load(std::memory_order_relaxed);
    const u64 length = watched_size.load(std::memory_order_relaxed);
    return length != 0 && device_addr < begin + length && begin < device_addr + size;
}

#ifdef _MSC_VER
namespace {
constexpr unsigned long kDepth = 10;
using Stack = std::array<void*, kDepth>;

std::string Symbolise(void* address) {
    static std::unordered_map<void*, std::string> cache;
    if (const auto it = cache.find(address); it != cache.end()) {
        return it->second;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    });
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    std::string name = "?";
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement,
                    symbol)) {
        name = symbol->Name;
    }
    return cache.emplace(address, std::move(name)).first->second;
}
} // Anonymous namespace

void NoteStackTrace(const char* tag) {
    if (!StackTraceEnabled()) {
        return;
    }
    Stack stack{};
    // Skip this function only; the tagged caller is the first frame.
    CaptureStackBackTrace(1, kDepth, stack.data(), nullptr);

    static std::mutex mutex;
    static std::map<std::pair<const char*, Stack>, u32> counts;
    static auto window_start = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[{tag, stack}];
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - window_start).count();
    if (secs < 4.0) {
        return;
    }
    std::vector<std::pair<std::pair<const char*, Stack>, u32>> sorted(counts.begin(),
                                                                       counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
        std::string trace;
        for (void* frame : sorted[i].first.second) {
            if (!frame) {
                break;
            }
            trace += trace.empty() ? "" : " <- ";
            trace += Symbolise(frame);
        }
        LOG_ERROR(Render_Vulkan, "StackTrace {} {:.1f}s n={} {}", sorted[i].first.first, secs,
                  sorted[i].second, trace);
    }
    counts.clear();
    window_start = now;
}
#else
void NoteStackTrace(const char*) {}
#endif

} // namespace VideoCommon
