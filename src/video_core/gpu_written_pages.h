// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever.
// Switch 2 games can build command lists and macro parameters on the GPU (culling dispatches that
// write indirect draw records each frame). Those words exist only in host buffers until they are
// downloaded, and the copy of a command list taken at submission predates them: run from either,
// the draws use an older frame's results (Fortune's Weave: grass and cloaks vanishing and coming
// back, worse with a fast camera). DmaPusher reads entries on these pages from guest memory after
// flushing them. The bits never clear within a session: the dispatch that rewrites a page may
// still be queued on the GPU thread split's back when its front reads the command list.
// One bit per 4 KiB of the 34-bit device address space (512 KiB). From PokeDinner 006e27f4.
class GpuWrittenPages {
public:
    static constexpr u64 PageBits = 12;
    static constexpr u64 AddressBits = 34;

    static void Mark(u64 device_address, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 last = (std::min)((device_address + size - 1) >> PageBits, NumPages - 1);
        for (u64 page = device_address >> PageBits; page <= last; ++page) {
            auto& word = words[page / 64];
            const u64 bit = u64{1} << (page % 64);
            if ((word.load(std::memory_order_relaxed) & bit) == 0) {
                word.fetch_or(bit, std::memory_order_relaxed);
            }
        }
    }

    /// Whether any page of [device_address, device_address + size) was written by the GPU.
    [[nodiscard]] static bool Any(u64 device_address, u64 size) {
        if (size == 0) {
            return false;
        }
        const u64 first = device_address >> PageBits;
        const u64 last = (std::min)((device_address + size - 1) >> PageBits, NumPages - 1);
        for (u64 page = first; page <= last; ++page) {
            if ((words[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
                return true;
            }
        }
        return false;
    }

    /// A new emulation session starts: no page has been written by its GPU yet.
    static void Clear() {
        for (auto& word : words) {
            word.store(0, std::memory_order_relaxed);
        }
    }

private:
    static constexpr u64 NumPages = u64{1} << (AddressBits - PageBits);
    static inline std::array<std::atomic<u64>, NumPages / 64> words{};
};

} // namespace VideoCommon
