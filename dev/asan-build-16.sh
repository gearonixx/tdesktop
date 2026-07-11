#!/bin/bash
# 16-core incremental ASan build of the Telegram target.
# Reconfigures link flags with memory-reduction options so the heavy ASan
# final link doesn't OOM, then builds -j16 inside the centos_env container.
set -e
BIN=/home/x/c/tdesktop/out/Asan/Telegram
echo "=== [$(date '+%H:%M:%S')] asan-build-16: starting (-j16) ==="

docker run --rm -u "$(id -u)" \
  --cpus=16 --memory=26g --memory-swap=48g \
  -v /home/x/c/tdesktop:/usr/src/tdesktop \
  tdesktop-build:mz \
  bash -lc '
    set -e
    cd /usr/src/tdesktop/out
    cmake . -DCMAKE_EXE_LINKER_FLAGS="-fno-lto -static-libstdc++ -static-libgcc -static-libasan -pthread -Wl,--push-state,--no-as-needed,-ldl,--pop-state -Wl,--as-needed -Wl,-z,muldefs -Wl,--no-keep-memory -Wl,--reduce-memory-overheads"
    echo "--- building Telegram (Asan, -j16) ---"
    cmake --build . --config Asan --target Telegram -j 16
  '

echo "=== [$(date '+%H:%M:%S')] asan-build-16: docker exited ==="
if [ -s "$BIN" ] && head -c4 "$BIN" | grep -q ELF; then
  ls -la "$BIN"; echo "ASAN BINARY OK"
else
  echo "ASAN BINARY BAD (empty or non-ELF)"
fi
