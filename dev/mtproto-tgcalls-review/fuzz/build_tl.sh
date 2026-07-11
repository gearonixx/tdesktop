#!/usr/bin/env bash
# Build the MTProto TL-deserializer libFuzzer harness against system Qt6.
set -euo pipefail

ROOT=/home/x/c/tdesktop
HERE="$ROOT/dev/mtproto-tgcalls-review/fuzz"
OUT="$HERE/build"
mkdir -p "$OUT"

CXX=clang++
SAN="${SAN:-address}"
QT_INC=$(pkg-config --cflags Qt6Core)

INCS=(
  -I"$HERE/override"                  # FUZZ-ONLY: bounded vector count (get past alloc-DoS)
  -I"$ROOT/out/Telegram/gen"          # scheme.h
  -I"$ROOT/Telegram/SourceFiles"      # mtproto/core_types.h
  -I"$ROOT/Telegram/lib_tl"           # tl/*
  -I"$ROOT/Telegram/lib_base"         # base/*
  -I"$ROOT/Telegram/lib_rpl"          # rpl/*
  -I"$ROOT/Telegram/ThirdParty/GSL/include"
  -I"$ROOT/Telegram/ThirdParty/range-v3/include"
  -I"$ROOT/Telegram/ThirdParty/expected/include"
)

CXXFLAGS=(-std=c++20 -g -O1 -fno-omit-frame-pointer
  -fsanitize="$SAN",fuzzer-no-link -fno-sanitize-recover=all
  -DNDEBUG $QT_INC "${INCS[@]}")

echo "[1/3] compiling scheme.cpp (4.5MB, slow)..."
$CXX "${CXXFLAGS[@]}" -c "$ROOT/out/Telegram/gen/scheme.cpp" -o "$OUT/scheme.o" &
echo "[2/3] compiling tl_basic_types.cpp..."
$CXX "${CXXFLAGS[@]}" -c "$ROOT/Telegram/lib_tl/tl/tl_basic_types.cpp" -o "$OUT/tl_basic.o" &
wait

echo "[3/3] compiling + linking harness..."
$CXX "${CXXFLAGS[@]}" -fsanitize="$SAN",fuzzer \
  "$HERE/fuzz_tl.cpp" "$OUT/scheme.o" "$OUT/tl_basic.o" \
  $(pkg-config --libs Qt6Core) \
  -o "$OUT/fuzz_tl"

echo "OK -> $OUT/fuzz_tl"
