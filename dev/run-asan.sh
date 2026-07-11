#!/bin/bash
# Launch the ASan Telegram build with full sanitizer + app debug logging,
# reusing the existing out/Debug session (same account + proxy config).
set -u
REPO=/home/x/c/tdesktop
BIN="$REPO/out/Asan/Telegram"
WORKDIR="$REPO/out/Debug"                 # reuse existing tdata (proxy/account)
LOGDIR="$REPO/dev/asan-run"
mkdir -p "$LOGDIR"

if [ ! -x "$BIN" ]; then echo "ERROR: $BIN missing/not executable"; exit 1; fi

# 1) Stop the currently-running non-ASan out/Debug instance to release the tdata lock.
echo "=== [$(date '+%H:%M:%S')] stopping existing out/Debug Telegram instance(s) ==="
pkill -TERM -f "$REPO/out/Debug/Telegram" 2>/dev/null || true
for i in $(seq 1 20); do
  pgrep -f "$REPO/out/Debug/Telegram" >/dev/null || break
  sleep 0.5
done
pkill -KILL -f "$REPO/out/Debug/Telegram" 2>/dev/null || true
sleep 1

# 2) ASan runtime options: report every error, keep going logs verbose, write to file.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:abort_on_error=0:print_stats=1:print_legend=1:verbosity=1:handle_segv=1:handle_abort=1:strict_string_checks=1:log_path=$LOGDIR/asan"
# Symbolizer for readable frames (llvm-symbolizer present on host).
export ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer 2>/dev/null || echo /usr/bin/llvm-symbolizer)"

echo "=== [$(date '+%H:%M:%S')] launching ASan Telegram ==="
echo "    BIN=$BIN"
echo "    -workdir $WORKDIR  -debug"
echo "    ASAN_OPTIONS=$ASAN_OPTIONS"

# 3) Launch. -debug -> gDebugMode verbose app logs (workdir/DebugLogs + log.txt).
#    stdout/stderr (incl. any ASan report echoed to stderr) tee'd to run log.
"$BIN" -workdir "$WORKDIR" -debug > "$LOGDIR/stdout.log" 2>&1 &
APP_PID=$!
echo "launched ASan Telegram pid $APP_PID" | tee "$LOGDIR/launch.info"
echo "$APP_PID" > "$LOGDIR/app.pid"
