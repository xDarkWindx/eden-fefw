// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include "common/logging.h"
#include "video_core/list_diag.h"

namespace VideoCommon::ListDiag {

namespace {
void LogLine(const std::string& line) {
    LOG_INFO(HW_GPU, "{}", line);
}
} // namespace

void InstallLogSink() {
    g_sink.store(&LogLine, std::memory_order_release);
}

} // namespace VideoCommon::ListDiag
