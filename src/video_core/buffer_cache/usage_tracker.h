// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

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
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
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
        const size_t first = static_cast<size_t>(blocks.first_page);
        const size_t last = static_cast<size_t>((std::min<u64>)(blocks.last_page, pages.size() - 1));
        dirty_begin = (std::min)(dirty_begin, first);
        dirty_end = (std::max)(dirty_end, last + 1);
        const u64 first_mask = FirstMask(blocks);
        const u64 last_mask = LastMask(blocks, last);
        if (first == last) {
            pages[first] |= first_mask & last_mask;
            return;
        }
        // A mask only on the first and the last word; the words in between are whole, with no branch per word.
        pages[first] |= first_mask;
        std::fill(pages.begin() + static_cast<std::ptrdiff_t>(first) + 1,
                  pages.begin() + static_cast<std::ptrdiff_t>(last), ~u64{0});
        pages[last] |= last_mask;
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const Blocks blocks = BlocksOf(offset, size);
        if (pages.empty() || blocks.last_page >= pages.size()) {
            return true; // a non-empty range past the extent: unknown counts as in use
        }
        const size_t first = static_cast<size_t>(blocks.first_page);
        const size_t last = static_cast<size_t>(blocks.last_page);
        const u64 first_mask = FirstMask(blocks);
        const u64 last_mask = LastMask(blocks, last);
        if (first == last) {
            return (pages[first] & first_mask & last_mask) != 0;
        }
        if ((pages[first] & first_mask) != 0) {
            return true;
        }
        for (size_t page = first + 1; page < last; ++page) {
            if (pages[page] != 0) {
                return true; // the words in between are whole: any bit is a used block of the range
            }
        }
        return (pages[last] & last_mask) != 0;
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

    /// The bits of the FIRST word that belong to the range: from the bit of its first block up. The shift is in 0..63.
    static u64 FirstMask(const Blocks& blocks) noexcept {
        return ~u64{0} << static_cast<unsigned>(blocks.first_block & 63);
    }

    /// The bits of the word `page` (the last one of the intersection) that belong to the range: up to the bit of its last
    /// block when `page` is the last word of the range, all of them when the range goes on past `page` (the extent cut it).
    /// `63 - hi` is in 0..63.
    static u64 LastMask(const Blocks& blocks, size_t page) noexcept {
        if (page != blocks.last_page) {
            return ~u64{0};
        }
        return ~u64{0} >> (63 - static_cast<unsigned>(blocks.last_block & 63));
    }

    struct Range {
        u64 offset;
        u64 size;
    };
    static constexpr Range NO_RANGE{(std::numeric_limits<u64>::max)(), 0};

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
