// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_finish_trace.h"

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
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#endif

namespace Vulkan {

#ifdef _MSC_VER
namespace {
constexpr unsigned long kDepth = 8;
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

void NoteFinishCaller() {
    static const bool enabled = std::getenv("EDEN_FINISH_TRACE") != nullptr;
    if (!enabled) {
        return;
    }
    Stack stack{};
    // Skip this function and Scheduler::Finish.
    CaptureStackBackTrace(2, kDepth, stack.data(), nullptr);

    static std::mutex mutex;
    static std::map<Stack, unsigned> counts;
    static auto window_start = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[stack];
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - window_start).count();
    if (secs < 4.0) {
        return;
    }
    std::vector<std::pair<Stack, unsigned>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
        std::string trace;
        for (void* frame : sorted[i].first) {
            if (!frame) {
                break;
            }
            trace += trace.empty() ? "" : " <- ";
            trace += Symbolise(frame);
        }
        LOG_ERROR(Render_Vulkan, "FinishTrace {:.1f}s n={} {}", secs, sorted[i].second, trace);
    }
    counts.clear();
    window_start = now;
}
#else
void NoteFinishCaller() {}
#endif

} // namespace Vulkan
