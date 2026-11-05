// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// In the emulator, video_core/list_diag.cpp installs the log sink (the emulator's log). Here the
// lines go to the standard error, which is what `Emit` does without a sink.
#include "video_core/list_diag.h"

namespace VideoCommon::ListDiag {
void InstallLogSink() {}
} // namespace VideoCommon::ListDiag
