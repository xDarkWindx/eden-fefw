// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"

namespace VideoCommon {

/// TEMPORARY switch, read once at the start: EDEN_USAGE_TRACKER_OLD=1 selects the tracker of before T4.1. It exists ONLY for
/// the user's A/B of the new tracker and WILL BE DELETED after it, with the new one left always on; it is not a permanent
/// option. The old mode keeps a KNOWN undefined behaviour (a shift by 64 for ranges smaller than 64 bytes or ending on
/// a 4 KiB edge) and false negatives (docs/t4-coherence-fix-plan.md, 9.2).
[[nodiscard]] inline bool ParseUsageTrackerOld(const char* value) {
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}
[[nodiscard]] inline bool UsageTrackerOldMode() {
    static const bool old_mode = ParseUsageTrackerOld(std::getenv("EDEN_USAGE_TRACKER_OLD"));
    return old_mode;
}

/// Which 64-byte blocks of a buffer a draw or a copy used since the last Reset, one bit per block and one 64-bit word per
/// 4 KiB. Contract (docs/t4-coherence-fix-plan.md, 9.1): a block touched by a non-empty range is marked by Track and is
/// recognised by IsUsed, including ranges that cross 64 B, cross the 4 KiB word, end exactly on an edge, or pass the
/// extent (IsUsed answers true for those). Tracking may be conservative (more blocks), never less. A range of
/// size zero touches nothing. No undefined shift.
class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    /// `old_mode` is fixed for the life of the tracker: Track and IsUsed use the same calculation (never mixed).
    explicit UsageTracker(size_t size, bool old_mode_ = UsageTrackerOldMode()) : old_mode{old_mode_} {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    [[nodiscard]] bool IsOldMode() const noexcept {
        return old_mode;
    }

    void Reset() noexcept {
        recent.fill(NO_RANGE);
        // Only the pages tracked since the last reset can be set.
        if (dirty_begin < dirty_end) {
            std::fill(pages.begin() + dirty_begin, pages.begin() + dirty_end, 0ULL);
        }
        dirty_begin = (std::numeric_limits<size_t>::max)();
        dirty_end = 0;
    }

    void Track(u64 offset, u64 size) noexcept {
        if (old_mode) {
            TrackOld(offset, size);
            return;
        }
        if (size == 0) {
            return; // touches no block; handled before any `size - 1`
        }
        // Consecutive draws keep binding the same ranges (a vertex buffer can span tens of MiB);
        // tracking a range again changes nothing until the next reset.
        for (const Range& range : recent) {
            if (range.offset == offset && range.size == size) {
                return;
            }
        }
        recent[recent_next++ % recent.size()] = Range{offset, size};
        const Blocks blocks = BlocksOf(offset, size);
        // Only the intersection with the pages that exist; none: nothing is touched, dirty_* stay as they are.
        if (pages.empty() || blocks.first_page >= pages.size()) {
            return;
        }
        const size_t last = static_cast<size_t>((std::min<u64>)(blocks.last_page, pages.size() - 1));
        dirty_begin = (std::min)(dirty_begin, static_cast<size_t>(blocks.first_page));
        dirty_end = (std::max)(dirty_end, last + 1);
        for (size_t page = static_cast<size_t>(blocks.first_page); page <= last; ++page) {
            // Whole words in the middle: no work per block.
            pages[page] |= PageMask(blocks, page);
        }
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        if (old_mode) {
            return IsUsedOld(offset, size);
        }
        if (size == 0) {
            return false;
        }
        const Blocks blocks = BlocksOf(offset, size);
        if (pages.empty() || blocks.last_page >= pages.size()) {
            return true; // a non-empty range past the extent: unknown counts as in use
        }
        for (size_t page = static_cast<size_t>(blocks.first_page); page <= static_cast<size_t>(blocks.last_page); ++page) {
            if ((pages[page] & PageMask(blocks, page)) != 0) {
                return true;
            }
        }
        return false;
    }

private:
    struct Blocks {
        u64 first_block;
        u64 last_block;
        u64 first_page;
        u64 last_page;
    };

    /// The blocks of 64 B a non-empty range touches: from the block of its first byte to the block of its last byte
    /// (saturated: `offset + (size - 1)` does not wrap).
    static Blocks BlocksOf(u64 offset, u64 size) noexcept {
        const u64 last_byte = (size - 1) > (std::numeric_limits<u64>::max)() - offset
                                  ? (std::numeric_limits<u64>::max)()
                                  : offset + (size - 1);
        Blocks blocks;
        blocks.first_block = offset >> BUFFER_BYTES_PER_BITSHIFT;
        blocks.last_block = last_byte >> BUFFER_BYTES_PER_BITSHIFT;
        blocks.first_page = blocks.first_block >> 6; // 64 blocks per word
        blocks.last_page = blocks.last_block >> 6;
        return blocks;
    }

    /// The bits of one word that belong to the range: `lo` and `hi` are in 0..63, so no shift is 64 or more.
    static u64 PageMask(const Blocks& blocks, size_t page) noexcept {
        if (page != blocks.first_page && page != blocks.last_page) {
            return ~u64{0};
        }
        const unsigned lo = page == blocks.first_page ? static_cast<unsigned>(blocks.first_block & 63) : 0;
        const unsigned hi = page == blocks.last_page ? static_cast<unsigned>(blocks.last_block & 63) : 63;
        return (~u64{0} << lo) & (~u64{0} >> (63 - hi));
    }

    // ---- The tracker of before T4.1 (EDEN_USAGE_TRACKER_OLD=1), kept as it was: it has an undefined shift by 64 for
    // ---- num_bits == 0 and misses the partial blocks (docs/t4-coherence-fix-plan.md, 9.2). To be deleted with the switch.
    void TrackOld(u64 offset, u64 size) noexcept {
        // Consecutive draws keep binding the same ranges (a vertex buffer can span tens of MiB);
        // tracking a range again changes nothing until the next reset.
        for (const Range& range : recent) {
            if (range.offset == offset && range.size == size) {
                return;
            }
        }
        recent[recent_next++ % recent.size()] = Range{offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        dirty_begin = (std::min)(dirty_begin, page);
        dirty_end = (std::max)(dirty_end, page_end + 1);
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsedOld(u64 offset, u64 size) const noexcept {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        pages[page] |= (~u64{0} & mask) << first_bit;
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        const size_t mask2 = (~u64{0} & mask) << first_bit;
        return (pages[page] & mask2) != 0;
    }

    struct Range {
        u64 offset;
        u64 size;
    };
    static constexpr Range NO_RANGE{(std::numeric_limits<u64>::max)(), 0};

    bool old_mode;
    std::vector<u64> pages;
    /// A draw can bind more than four ranges of one buffer (storage buffers carved from one pool):
    /// with too few slots each draw forgot the others and filled their pages again.
    std::array<Range, 16> recent = [] {
        std::array<Range, 16> ranges;
        ranges.fill(NO_RANGE);
        return ranges;
    }();
    size_t recent_next{};
    size_t dirty_begin{(std::numeric_limits<size_t>::max)()};
    size_t dirty_end{};
};

} // namespace VideoCommon
