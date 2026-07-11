#!/usr/bin/env bash
# Campaign 3: 2 fuzzers x 3 directions = 6 AFL++ nodes, fresh out3/ dirs.
#  A (broad)  : general seeds, explore schedule
#  B (dict)   : token-aware mutation with tex.dict, coe schedule
#  C (struct) : deep-recursion + matrix/env/array seeds, MOpt+exploit (N1/N2 area)
set -euo pipefail
HERE=/home/x/c/tdesktop/dev/microtex-fuzz
BIN=$HERE/build_afl/afl_microtex
ROOT=$HERE/out3

export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
export AFL_SKIP_CPUFREQ=1
export AFL_AUTORESUME=1
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0:handle_abort=2:allocator_may_return_null=1

launch() { # outdir masterflags secflags seeddir extra
  local out=$1 mflag=$2 sflag=$3 seed=$4 extra=$5
  mkdir -p "$out"
  local IN=$seed; [ -d "$out/m/queue" ] && IN=-
  AFL_AUTORESUME=1 setsid afl-fuzz -i "$IN" -o "$out" -m none -t 2000+ $extra \
    $mflag m -- "$BIN" >"$HERE/c3_$(basename "$out")_m.log" 2>&1 &
  echo "  $(basename "$out")/m pid $!"
  IN=$seed; [ -d "$out/s/queue" ] && IN=-
  AFL_AUTORESUME=1 setsid afl-fuzz -i "$IN" -o "$out" -m none -t 2000+ $extra \
    $sflag s -- "$BIN" >"$HERE/c3_$(basename "$out")_s.log" 2>&1 &
  echo "  $(basename "$out")/s pid $!"
}

echo "== Direction A: broad havoc =="
launch "$ROOT/a" "-M" "-S" "$HERE/seeds"        "-p explore"
echo "== Direction B: dictionary-guided =="
launch "$ROOT/b" "-M" "-S" "$HERE/seeds"        "-p coe -x $HERE/tex.dict"
echo "== Direction C: structural / matrix-env (MOpt) =="
launch "$ROOT/c" "-M" "-S" "$HERE/seeds_struct" "-p exploit -L 0"

sleep 4
echo "=== running afl nodes ==="
pgrep -a afl-fuzz | sed 's/ -- .*//'
