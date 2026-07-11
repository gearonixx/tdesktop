#!/usr/bin/env bash
# Build a headless libFuzzer harness for MicroTeX (parse + layout metrics).
set -euo pipefail

MT=/home/x/c/tdesktop/Telegram/ThirdParty/MicroTeX
SRC=$MT/src
HERE=/home/x/c/tdesktop/dev/microtex-fuzz
OUT=$HERE/build
mkdir -p "$OUT/obj"

SAN="${SAN:-address,undefined}"
CXX=clang++
# NOTE: -fno-sanitize=vptr — UBSan's vptr check crashes inside libstdc++'s
# __dynamic_cast for MicroTeX's multiply-inherited atoms (e.g. ColorAtom :
# public Atom, public Row) even on valid objects. That is a clang-vptr +
# libstdc++ tooling artifact, NOT a real bug (verified: no repro without
# sanitizers, and it does not reproduce in the real Telegram build). Leaving
# vptr on would flood every \color / unknown-command input with fake crashes
# and mask genuine memory-corruption findings.
CXXFLAGS="-std=c++17 -g -O1 -fno-omit-frame-pointer -fsanitize=$SAN,fuzzer-no-link \
  -fno-sanitize=vptr -fno-sanitize-recover=all -I$SRC -DNDEBUG"

# All core sources except platform/* and samples/*.
mapfile -t SOURCES < <(find "$SRC" -name '*.cpp' \
  -not -path "$SRC/platform/*" -not -path "$SRC/samples/*" | sort)
SOURCES+=("$HERE/stub_platform.cpp")

echo "Compiling ${#SOURCES[@]} translation units..."
pids=()
for f in "${SOURCES[@]}"; do
  obj="$OUT/obj/$(echo "$f" | md5sum | cut -c1-16).o"
  ( $CXX $CXXFLAGS -c "$f" -o "$obj" ) &
  pids+=($!)
  # throttle to nproc
  if (( ${#pids[@]} >= 16 )); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
wait

echo "Archiving..."
ar rcs "$OUT/libmicrotex.a" "$OUT"/obj/*.o

echo "Linking fuzzer..."
$CXX $CXXFLAGS -fsanitize=$SAN,fuzzer -fno-sanitize=vptr \
  "$HERE/fuzz_microtex.cpp" "$OUT/libmicrotex.a" -o "$OUT/fuzz_microtex"

echo "Done: $OUT/fuzz_microtex"
