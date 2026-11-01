// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_FINISH_TRACE: counts call stacks per tag and logs the most frequent ones, with symbol
/// names, every 4 s. Needs the PDB next to the executable to print names. MSVC only; the
/// functions are no-ops elsewhere.
bool StackTraceEnabled();
void NoteStackTrace(const char* tag);

/// Device range whose writers are traced (tag "HandleWrite"), set where a GPU-written range has
/// to be downloaded for the CPU.
void WatchStackTraceRange(u64 device_addr, u64 size);
bool InWatchedStackTraceRange(u64 device_addr, u64 size);

} // namespace VideoCommon
