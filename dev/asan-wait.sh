BIN=/home/x/c/tdesktop/out/Asan/Telegram
LOG=/home/x/c/tdesktop/dev/asan-build.log
# Wait for the current build container to finish (present now).
while docker ps --format '{{.Image}}' | grep -q tdesktop-build; do sleep 8; done
sleep 2
if [ -x "$BIN" ]; then
  echo "=== BUILD SUCCEEDED ==="; ls -la "$BIN"
else
  echo "=== BUILD CYCLE FAILED — recent compile errors ==="
  grep -nE 'error:|FAILED: ' "$LOG" | tail -12
fi
