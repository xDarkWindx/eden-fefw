// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon {

/// Parts of the High (Accurate) GPU accuracy level that EDEN_GPU_HIGH_PARTS can turn on alone
/// while the level is Low (Fast), to find which one a game needs without paying for all of them.
/// The variable takes a decimal or 0x-prefixed mask of these bits.
enum class HighAccuracyPart : u32 {
    MacroParameters = 1 << 0,   ///< Re-read macro parameters that live in GPU memory.
    TextureFlush = 1 << 1,      ///< Report GPU-written textures to CPU reads (flush them).
    FlushOnInvalidate = 1 << 2, ///< Flush a region before a guest write invalidates it.
    QueryOrdering = 1 << 3,     ///< Write semaphore payloads in GPU order, not at once.
};

[[nodiscard]] inline u32 HighAccuracyPartsMask() {
    static const u32 mask = [] {
        const char* value = std::getenv("EDEN_GPU_HIGH_PARTS");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0U;
    }();
    return mask;
}

/// Whether this part of the High accuracy level is on: at High, or asked for at Low.
[[nodiscard]] inline bool IsHighAccuracyPart(HighAccuracyPart part) {
    return Settings::IsGPULevelHigh() || (HighAccuracyPartsMask() & static_cast<u32>(part)) != 0;
}

/// Switch 2 (native) channels re-read macro parameters that the GPU may have written, at any
/// accuracy level: Donkey Kong Bananza's terrain draws take counts written by compute programs and
/// were not drawn at Fast without it. EDEN_NX2_MACRO_REFRESH=0 turns this off.
[[nodiscard]] inline bool NativeMacroRefresh() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MACRO_REFRESH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

} // namespace VideoCommon
