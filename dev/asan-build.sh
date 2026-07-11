#!/bin/bash
# Reconfigure the out/ multi-config tree to add an Asan config (keeping DWARF),
# then build the Telegram target with ASan on 8 cores, inside the centos_env container.
set -e
echo "=== [$(date '+%H:%M:%S')] starting ASan reconfigure ==="

docker run --rm -u "$(id -u)" \
  --cpus=8 --memory=16g --memory-swap=32g \
  -v /home/x/c/tdesktop:/usr/src/tdesktop \
  tdesktop-build:mz \
  bash -lc '
    set -e
    cd /usr/src/tdesktop/out
    echo "--- cmake reconfigure (add Asan, drop --strip-debug) ---"
    cmake . \
      -DCMAKE_CONFIGURATION_TYPES="Debug;Release;RelWithDebInfo;MinSizeRel;Asan" \
      -DCMAKE_EXE_LINKER_FLAGS="-fno-lto -static-libstdc++ -static-libgcc -static-libasan -pthread -Wl,--push-state,--no-as-needed,-ldl,--pop-state -Wl,--as-needed -Wl,-z,muldefs"
    echo "--- building Telegram (Asan, -j8) ---"
    cmake --build . --config Asan --target Telegram -j 8
  '

echo "=== [$(date '+%H:%M:%S')] build command exited ==="
ls -la /home/x/c/tdesktop/out/Asan/Telegram 2>/dev/null && echo "ASAN BINARY READY" || echo "ASAN BINARY MISSING"
