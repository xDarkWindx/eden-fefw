// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

/// EDEN_NO_TEXTURE_DEDUPE disables the per-draw texture lookup memos (for A/B comparisons).
inline bool TextureLookupDedupeEnabled() {
    static const bool enabled = std::getenv("EDEN_NO_TEXTURE_DEDUPE") == nullptr;
    return enabled;
}

/// Direct-mapped memo of descriptor index -> resolved id, valid until the next Reset. Buffer-backed
/// texture arrays bind every record of their table on every draw, and records often repeat the same
/// texture or sampler index; within one draw a repeated index always resolves to the same id.
template <typename Id, size_t Size, typename Key = u32>
class LookupMemo {
    static_assert((Size & (Size - 1)) == 0);
    static_assert(sizeof(Key) == 4 || sizeof(Key) == 8);

public:
    /// Starts a new scope; ids stored before are no longer returned.
    void Reset(const char* name) noexcept {
        if (++generation == 0) {
            entries.fill({});
            generation = 1;
        }
        Report(name);
    }

    [[nodiscard]] const Id* Find(Key key) noexcept {
        ++lookups;
        const Entry& entry = entries[Slot(key)];
        if (entry.generation == generation && entry.key == key) {
            ++hits;
            return &entry.id;
        }
        return nullptr;
    }

    void Store(Key key, Id id) noexcept {
        entries[Slot(key)] = Entry{key, generation, id};
    }

private:
    static size_t Slot(Key key) noexcept {
        if constexpr (sizeof(Key) == 8) {
            return static_cast<size_t>((key ^ (key >> 29)) & (Size - 1));
        } else {
            return static_cast<size_t>(key & (Size - 1));
        }
    }

    struct Entry {
        Key key{};
        u32 generation{};
        Id id{};
    };

    /// EDEN_GPU_PROFILE: logs the hit rate every 4 s.
    void Report(const char* name) noexcept {
        static const bool enabled = std::getenv("EDEN_GPU_PROFILE") != nullptr;
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start < std::chrono::seconds{4}) {
            return;
        }
        LOG_ERROR(HW_GPU, "LookupMemo {} lookups={} hits={} ({:.1f}%)", name, lookups, hits,
                  lookups ? hits * 100.0 / lookups : 0.0);
        lookups = 0;
        hits = 0;
        window_start = now;
    }

    std::array<Entry, Size> entries{};
    u32 generation{1};
    u64 lookups{};
    u64 hits{};
    std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
};

} // namespace VideoCommon
