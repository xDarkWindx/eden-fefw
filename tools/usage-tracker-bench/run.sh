#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds and runs the Track / IsUsed microbenchmark (new tracker against the frozen old one). No sanitizers: the old tracker
# is called only with ranges where it is defined. Usage: tools/usage-tracker-bench/run.sh
here=$(cd "$(dirname "$0")" && pwd)
out=${USAGE_BENCH_OUT:-"${TMPDIR:-/tmp}/usage-tracker-bench"}
mkdir -p "$out"
CXX=${CXX:-g++}
$CXX -std=c++20 -O2 -Wall -Wextra -Werror -I "$here/../../src" "$here/bench.cpp" -o "$out/bench"
"$out/bench"
