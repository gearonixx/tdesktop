#!/usr/bin/env bash
# Launch a multi-core AFL++ campaign against the MicroTeX persistent harness.
# Resumes the existing out/ campaign (corpus preserved). Usage: ./run_afl.sh [N_SECONDARIES]
set -euo pipefail
HERE=/home/x/c/tdesktop/dev/microtex-fuzz
BIN=$HERE/build_afl/afl_microtex
OUT=$HERE/out
SEEDS=$HERE/seeds
NSEC=${1:-6}   # secondaries in addition to the master

# ASan build: don't let core_pattern / mem limit abort the fuzzer.
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
export AFL_SKIP_CPUFREQ=1
export AFL_AUTORESUME=1
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0:handle_abort=2:allocator_may_return_null=1

# Fresh input dir only if there is no prior campaign to resume.
if [ -d "$OUT/m0/queue" ]; then IN=-; else IN=$SEEDS; fi

start() { # name flag
  local name=$1 flag=$2
  AFL_AUTORESUME=1 setsid afl-fuzz -i "$IN" -o "$OUT" -m none -t 2000+ \
    $flag "$name" -- "$BIN" >"$HERE/afl_${name}.log" 2>&1 &
  echo "  started $name (pid $!)"
}

echo "Launching master + $NSEC secondaries (resume=$([ "$IN" = - ] && echo yes || echo no))"
start m0 -M
for i in $(seq 1 "$NSEC"); do start "s$i" -S; done
sleep 3
echo "=== running fuzzers ==="
pgrep -a afl-fuzz | sed 's/ -- .*//'
