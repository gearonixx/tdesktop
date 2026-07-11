#!/bin/bash
# Final ASan link failed with "R_X86_64_32 against .debug_info" (DWARF32 4GB
# overflow from statically linking full-debug Qt libs + our objects).
# Relink with --strip-debug so ld drops the .debug_* sections (and their
# 32-bit relocations) from the output. .symtab is kept, so ASan backtraces
# still resolve function names. All objects are cached -> link only.
set -e
BIN=/home/x/c/tdesktop/out/Asan/Telegram
echo "=== [$(date '+%H:%M:%S')] relink-stripdebug: starting (-j16) ==="

docker run --rm -u "$(id -u)" \
  --cpus=16 --memory=26g --memory-swap=48g \
  -v /home/x/c/tdesktop:/usr/src/tdesktop \
  tdesktop-build:mz \
  bash -lc '
    set -e
    cd /usr/src/tdesktop/out
    cmake . -DCMAKE_EXE_LINKER_FLAGS="-fno-lto -static-libstdc++ -static-libgcc -static-libasan -pthread -Wl,--push-state,--no-as-needed,-ldl,--pop-state -Wl,--as-needed -Wl,-z,muldefs -Wl,--no-keep-memory -Wl,--reduce-memory-overheads -Wl,--strip-debug"
    echo "--- linking Telegram (Asan, strip-debug) ---"
    cmake --build . --config Asan --target Telegram -j 16
  '

echo "=== [$(date '+%H:%M:%S')] relink-stripdebug: docker exited ==="
if [ -s "$BIN" ] && head -c4 "$BIN" | grep -q ELF; then
  ls -la "$BIN"; echo "ASAN BINARY OK"
else
  echo "ASAN BINARY BAD (empty or non-ELF)"
fi
