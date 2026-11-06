// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The mark of the diagnostic's own on the GPU threads (docs/list-diag-design.md, 3.8.6): posted where the back
// thread and the front thread use the rasterizer. A thread with no mark is Unknown, and a comparison made on it
// counts as other/unknown, never as the front. Independent of BackStats (which exists only with the statistics).
#pragma once

#include "common/common_types.h"

namespace VideoCommon::ListDiag {

enum class ThreadRole : u8 { Unknown, Back, Front };

inline thread_local ThreadRole t_role = ThreadRole::Unknown;

inline void MarkThread(ThreadRole role) {
    t_role = role;
}

} // namespace VideoCommon::ListDiag
