#!/usr/bin/env bash
# Sleep for the 3h timebox, then stop campaign 3 and triage NEW crashes only.
set -uo pipefail
HERE=/home/x/c/tdesktop/dev/microtex-fuzz
NPBIN=$HERE/build_afl2/afl_microtex_np      # non-persistent = clean per-input repro
DUR=${1:-10800}                              # 3 hours
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1:handle_abort=2

sleep "$DUR"

echo "=== [$(date)] timebox reached, stopping afl nodes ==="
pkill -TERM -x afl-fuzz; sleep 3; pkill -KILL -x afl-fuzz 2>/dev/null

# Collect crashes across all 6 nodes
mkdir -p "$HERE/triage3/uniq"
n=0
for d in a b c; do for node in m s; do
  qd="$HERE/out3/$d/$node/crashes"
  [ -d "$qd" ] || continue
  for f in "$qd"/id:*; do
    [ -e "$f" ] || continue
    h=$(md5sum "$f" | cut -c1-16)
    dst="$HERE/triage3/uniq/${d}${node}_${h}"
    [ -e "$dst" ] || { cp "$f" "$dst"; n=$((n+1)); }
  done
done; done
echo "collected $n unique-by-content crash files into triage3/uniq"

# Classify each against the CURRENT tree via the np (single-shot) binary
classify() {
  f="$1"
  out=$(timeout 12 "$NPBIN" "$f" 2>&1)
  etype=$(printf '%s' "$out" | grep -oE "AddressSanitizer: [a-zA-Z0-9_-]+|runtime error: [^\"]+|stack-overflow|out-of-memory" | head -1)
  [ -z "$etype" ] && { [ -z "$out" ] && etype="TIMEOUT_or_SILENT" || etype="OTHER"; }
  frame=$(printf '%s' "$out" | grep -oE "in tex::[A-Za-z0-9_:~]+" | grep -vE "runOne|toWide" | head -1)
  printf '%s | %s || %s\n' "$etype" "$frame" "$f"
}
export -f classify; export NPBIN ASAN_OPTIONS
ls "$HERE"/triage3/uniq/* 2>/dev/null | xargs -P 12 -I{} bash -c 'classify "{}"' > "$HERE/triage3/summary.txt" 2>/dev/null

echo "=== crash-class histogram (count | signature) ==="
sed -E 's/ \|\| .*//' "$HERE/triage3/summary.txt" | sort | uniq -c | sort -rn | tee "$HERE/triage3/histogram.txt"
echo "=== DONE campaign 3 triage ==="
