#!/bin/bash
# Re-run only the final ASan link (all 229 objects are cached) with lower ld
# memory pressure, now that host RAM has freed up.
set -e
BIN=/home/x/c/tdesktop/out/Asan/Telegram
echo "=== [$(date '+%H:%M:%S')] relink: reconfigure link flags (+--no-keep-memory) ==="
docker run --rm -u "$(id -u)" \
  --cpus=8 --memory=24g --memory-swap=40g \
  -v /home/x/c/tdesktop:/usr/src/tdesktop \
  tdesktop-build:mz \
  bash -lc '
    set -e
    cd /usr/src/tdesktop/out
    cmake . -DCMAKE_EXE_LINKER_FLAGS="-fno-lto -static-libstdc++ -static-libgcc -static-libasan -pthread -Wl,--push-state,--no-as-needed,-ldl,--pop-state -Wl,--as-needed -Wl,-z,muldefs -Wl,--no-keep-memory -Wl,--reduce-memory-overheads"
    echo "--- linking Telegram (Asan) ---"
    cmake --build . --config Asan --target Telegram -j 8
  '
echo "=== [$(date '+%H:%M:%S')] relink: docker exited ==="
if [ -s "$BIN" ] && head -c4 "$BIN" | grep -q ELF; then
  ls -la "$BIN"; echo "ASAN BINARY OK"
else
  echo "ASAN BINARY BAD (empty or non-ELF)"
fi
