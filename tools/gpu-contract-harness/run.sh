#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds the contract harness three times (optimised, ThreadSanitizer,
# AddressSanitizer + UndefinedBehaviorSanitizer) and runs each build.
# Any sanitizer report makes the run fail (halt_on_error / exitcode).
#
# Usage: tools/gpu-contract-harness/run.sh [random-runs] [forced-runs]
# Defaults: 2000 random and 200 forced runs per case, rule and schedule for the
# optimised build; the sanitizer builds use a tenth of that.

here=$(cd "$(dirname "$0")" && pwd)
out=${HARNESS_OUT:-"${TMPDIR:-/tmp}/gpu-contract-harness"}
mkdir -p "$out"

random=${1:-2000}
forced=${2:-200}
san_random=$((random / 10))
san_forced=$((forced / 10))
[ "$san_random" -gt 0 ] || san_random=1
[ "$san_forced" -gt 0 ] || san_forced=1

CXX=${CXX:-g++}
common="-std=c++20 -Wall -Wextra -Wno-missing-field-initializers -Werror -pthread"
src="$here/cases.cpp $here/model.cpp"

echo "== build: -O2"
$CXX $common -O2 $src -o "$out/harness"
echo "== build: -fsanitize=thread"
$CXX $common -O1 -g -fsanitize=thread $src -o "$out/harness-tsan"
echo "== build: -fsanitize=address,undefined"
$CXX $common -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $src -o "$out/harness-asan"

echo
echo "== run: -O2 ($random random, $forced forced)"
"$out/harness" --random "$random" --forced "$forced"

echo
echo "== run: tsan ($san_random random, $san_forced forced)"
TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}" \
    "$out/harness-tsan" --random "$san_random" --forced "$san_forced" --seed 2

echo
echo "== run: asan+ubsan ($san_random random, $san_forced forced)"
ASAN_OPTIONS="halt_on_error=1 detect_leaks=1 ${ASAN_OPTIONS:-}" \
UBSAN_OPTIONS="halt_on_error=1 print_stacktrace=1 ${UBSAN_OPTIONS:-}" \
    "$out/harness-asan" --random "$san_random" --forced "$san_forced" --seed 3

echo
echo "all three builds met every expectation without sanitizer reports"
