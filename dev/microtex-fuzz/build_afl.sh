#!/usr/bin/env bash
# Build AFL++ persistent-mode harness for MicroTeX (ASan+UBSan, no vptr).
set -euo pipefail
MT=/home/x/c/tdesktop/Telegram/ThirdParty/MicroTeX
SRC=$MT/src
HERE=/home/x/c/tdesktop/dev/microtex-fuzz
OUT=$HERE/build_afl
mkdir -p "$OUT/obj"

export AFL_USE_ASAN=1
export AFL_USE_UBSAN=1
CXX=afl-clang-fast++
# vptr disabled: clang-vptr + libstdc++ __dynamic_cast artifact on MicroTeX's
# multiply-inherited atoms (see build.sh note).
CXXFLAGS="-std=c++17 -g -O1 -fno-omit-frame-pointer -fno-sanitize=vptr -I$SRC -DNDEBUG"

mapfile -t SOURCES < <(find "$SRC" -name '*.cpp' \
  -not -path "$SRC/platform/*" -not -path "$SRC/samples/*" | sort)
SOURCES+=("$HERE/stub_platform.cpp" "$HERE/afl_microtex.cpp")

echo "Compiling ${#SOURCES[@]} TUs with $CXX ..."
pids=()
for f in "${SOURCES[@]}"; do
  obj="$OUT/obj/$(echo "$f" | md5sum | cut -c1-16).o"
  ( $CXX $CXXFLAGS -c "$f" -o "$obj" ) &
  pids+=($!)
  if (( ${#pids[@]} >= 16 )); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
wait

echo "Linking..."
$CXX $CXXFLAGS "$OUT"/obj/*.o -o "$OUT/afl_microtex"
echo "Done: $OUT/afl_microtex"
