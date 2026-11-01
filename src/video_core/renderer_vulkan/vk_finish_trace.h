// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

/// EDEN_FINISH_TRACE: records the call stack of every Scheduler::Finish (a full CPU wait on the
/// GPU) and logs the most frequent stacks, with symbol names, every 4 s. Needs the PDB next to
/// the executable to print names. MSVC only; a no-op elsewhere.
void NoteFinishCaller();

} // namespace Vulkan
