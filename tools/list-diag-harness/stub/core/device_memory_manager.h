// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Stand-in for core/device_memory_manager.h, so that the real
// core/gpu_dirty_memory_manager.h compiles outside the emulator (it only needs the page size).
#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace Core {
constexpr std::size_t DEVICE_PAGEBITS = 12ULL;
} // namespace Core
