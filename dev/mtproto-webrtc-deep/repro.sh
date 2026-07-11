#!/usr/bin/env bash
# Reproduces MTPROTO-3 (pre-auth AES-IGE alignment abort). Requires OpenSSL dev.
set -u
cd "$(dirname "$0")"

echo "### 1. Minimal proof: OpenSSL AES_ige_encrypt aborts on len % 16 != 0"
cc -w ige_test.c -o ige_test -lcrypto
echo "--- len=24 (4-aligned, NOT 16-aligned; passes the client gate) ---"
./ige_test 24; echo "exit=$?  (134 = SIGABRT)"
echo "--- len=32 (16-aligned, legitimate) ---"
./ige_test 32; echo "exit=$?"

echo
echo "### 2. Faithful model of dhParamsAnswered() gate + decrypt"
c++ -std=c++17 -w -g poc_dh_ige_abort.cpp -o poc -lcrypto
echo "--- BUGGY gate (current: encDHLen & 0x03) ---"
./poc buggy; echo "exit=$?  (134 = SIGABRT => remote pre-auth DoS)"
echo "--- FIXED gate (proposed: encDHLen & 0x0F) ---"
./poc fixed; echo "exit=$?  (0 => rejected before decrypt)"
