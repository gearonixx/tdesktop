#!/usr/bin/env bash
# One-command reproduction of F-2 (MTProto pre-auth handshake OOB read / remote DoS).
# Usage: bash repro.sh
set -u
cd "$(dirname "$0")"
CXX=${CXX:-clang++}
FL="-std=c++17 -g -O0 -fsanitize=address -fno-omit-frame-pointer"
echo "== building PoCs =="
$CXX $FL poc_notsecure_oob.cpp -o poc_notsecure_oob 2>/dev/null
$CXX $FL poc_respq_dos.cpp      -o poc_respq_dos      2>/dev/null
echo
echo "== F-2 minimal OOB (buggy must ASan-abort, fixed must pass) =="
ASAN_OPTIONS=detect_leaks=0 ./poc_notsecure_oob        >/dev/null 2>&1; echo "  buggy  -> exit $? (nonzero = OOB caught)"
ASAN_OPTIONS=detect_leaks=0 ./poc_notsecure_oob fixed  >/dev/null 2>&1; echo "  fixed  -> exit $? (0 = clean)"
echo
echo "== F-2 remote-DoS, ResPQ-faithful, 1MB packet =="
ASAN_OPTIONS=detect_leaks=0 ./poc_respq_dos            2>&1 | grep -E "overrun|heap-buffer-overflow|READ of size" | head -3
ASAN_OPTIONS=detect_leaks=0 ./poc_respq_dos            >/dev/null 2>&1; echo "  buggy  -> exit $? (nonzero = crash / DoS)"
ASAN_OPTIONS=detect_leaks=0 ./poc_respq_dos fixed      >/dev/null 2>&1; echo "  fixed  -> exit $? (0 = clean)"
