#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds src/tests/video_core/list_diag.cpp (the same Catch2 file the emulator's test target
# builds) three times (optimised, ThreadSanitizer, AddressSanitizer + UndefinedBehaviorSanitizer)
# and runs each build, each at both grains of the dirty manager (64 B default, 4 B experimental).
# It includes the REAL src/video_core/list_diag/*.h and src/core/gpu_dirty_memory_manager.h; the
# only stand-ins are a stub of core/device_memory_manager.h (the page size) and a log sink.
# Needs Catch2 3 (libcatch2-dev) and Boost (libboost-dev, for the real Common::RangeSet).
#
# Usage: tools/list-diag-harness/run.sh [stress-notices-per-core]
# Default: 3000 notices per core for the optimised build; the sanitizer builds use a fifth.

here=$(cd "$(dirname "$0")" && pwd)
out=${LIST_DIAG_OUT:-"${TMPDIR:-/tmp}/list-diag-harness"}
mkdir -p "$out"

notices=${1:-3000}
san_notices=$((notices / 5))
[ "$san_notices" -gt 0 ] || san_notices=1

CXX=${CXX:-g++}
common="-std=c++20 -Wall -Wextra -Werror -pthread -I $here/stub -I $here/../../src"
src="$here/../../src/tests/video_core/list_diag.cpp $here/sink_stub.cpp"
libs="-lCatch2Main -lCatch2"

echo "== build: -O2"
$CXX $common -O2 $src $libs -o "$out/ldh"
echo "== build: -fsanitize=thread"
$CXX $common -O1 -g -fsanitize=thread $src $libs -o "$out/ldh-tsan"
echo "== build: -fsanitize=address,undefined"
$CXX $common -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $src $libs -o "$out/ldh-asan"

# Each build runs the battery at BOTH grains of the dirty manager: the default (64 B, a transformation of 2 KiB) and the
# experimental one (EDEN_DIRTY_GRAIN=4: 4 B, 128 B). The tests whose subject is one grain construct it explicitly; the others
# follow the variable (docs/t4-coherence-fix-plan.md, revision 5).
run_both() {
    label=$1
    shift
    echo
    echo "== run: $label, grain 64 B (default)"
    env -u EDEN_DIRTY_GRAIN "$@"
    echo
    echo "== run: $label, grain 4 B (EDEN_DIRTY_GRAIN=4)"
    env EDEN_DIRTY_GRAIN=4 "$@"
}

run_both "-O2 ($notices stress notices per core)" env LIST_DIAG_STRESS_NOTICES="$notices" "$out/ldh"

run_both "tsan ($san_notices stress notices per core)" env LIST_DIAG_STRESS_NOTICES="$san_notices" \
    TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}" "$out/ldh-tsan"

run_both "asan+ubsan ($san_notices stress notices per core)" env LIST_DIAG_STRESS_NOTICES="$san_notices" \
    ASAN_OPTIONS="halt_on_error=1 detect_leaks=1 ${ASAN_OPTIONS:-}" \
    UBSAN_OPTIONS="halt_on_error=1 print_stacktrace=1 ${UBSAN_OPTIONS:-}" "$out/ldh-asan"

echo
echo "all three builds passed every test at both grains without sanitizer reports"
