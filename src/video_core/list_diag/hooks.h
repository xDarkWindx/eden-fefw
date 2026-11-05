// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

#include "common/common_types.h"
#include "core/gpu_dirty_memory_manager.h"

namespace VideoCommon::ListDiag {

/// What the emulator uses: no stop points, no test knobs, the steady clock in microseconds.
/// A test supplies its own type with the same members (a clock it moves, stop points that block,
/// the knobs `MutantDelta` and `NaiveNoLateCheck`) and `kTest = true`.
struct ProdHooks : Core::DirtyNoHooks {
    static u64 NowUs() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }
};

} // namespace VideoCommon::ListDiag
