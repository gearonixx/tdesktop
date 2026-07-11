#!/bin/bash
# Wait for any in-flight tdesktop-build container to exit, then run an
# incremental ASan build of the Telegram target (no reconfigure needed).
echo "=== [$(date '+%H:%M:%S')] resume: waiting for in-flight build container to exit ==="
while docker ps --format '{{.Image}}' | grep -q 'tdesktop-build'; do
  sleep 5
done
echo "=== [$(date '+%H:%M:%S')] resume: starting incremental ASan build (-j8) ==="

docker run --rm -u "$(id -u)" \
  --cpus=8 --memory=16g --memory-swap=32g \
  -v /home/x/c/tdesktop:/usr/src/tdesktop \
  tdesktop-build:mz \
  bash -lc 'cd /usr/src/tdesktop/out && cmake --build . --config Asan --target Telegram -j 8'

echo "=== [$(date '+%H:%M:%S')] resume: build command exited ==="
ls -la /home/x/c/tdesktop/out/Asan/Telegram 2>/dev/null && echo "ASAN BINARY READY" || echo "ASAN BINARY MISSING"
