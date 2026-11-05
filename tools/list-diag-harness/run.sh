#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds the ledger/lot harness three times (optimised, ThreadSanitizer,
# AddressSanitizer + UndefinedBehaviorSanitizer) and runs each build. Any failed check or
# sanitizer report makes the run fail.
#
# Usage: tools/list-diag-harness/run.sh [notices-per-core] [rounds]
# Defaults: 3000 notices per core and 3 rounds for the optimised build; the sanitizer builds
# use a fifth of the notices.

here=$(cd "$(dirname "$0")" && pwd)
out=${LIST_DIAG_OUT:-"${TMPDIR:-/tmp}/list-diag-harness"}
mkdir -p "$out"

notices=${1:-3000}
rounds=${2:-3}
san_notices=$((notices / 5))
[ "$san_notices" -gt 0 ] || san_notices=1

CXX=${CXX:-g++}
common="-std=c++20 -Wall -Wextra -Werror -pthread -I $here/stub -I $here/../../src"
src="$here/main.cpp"

echo "== build: -O2"
$CXX $common -O2 $src -o "$out/ldh"
echo "== build: -fsanitize=thread"
$CXX $common -O1 -g -fsanitize=thread $src -o "$out/ldh-tsan"
echo "== build: -fsanitize=address,undefined"
$CXX $common -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $src -o "$out/ldh-asan"

echo
echo "== run: -O2 ($notices notices per core, $rounds rounds)"
"$out/ldh" --notices "$notices" --rounds "$rounds"

echo
echo "== run: tsan ($san_notices notices per core)"
TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}" \
    "$out/ldh-tsan" --notices "$san_notices" --rounds 2

echo
echo "== run: asan+ubsan ($san_notices notices per core)"
ASAN_OPTIONS="halt_on_error=1 detect_leaks=1 ${ASAN_OPTIONS:-}" \
UBSAN_OPTIONS="halt_on_error=1 print_stacktrace=1 ${UBSAN_OPTIONS:-}" \
    "$out/ldh-asan" --notices "$san_notices" --rounds 2

echo
echo "all three builds met every expectation without sanitizer reports"
