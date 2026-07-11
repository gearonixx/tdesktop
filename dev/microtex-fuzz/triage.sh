#!/usr/bin/env bash
# Classify each unique crash by ASan/UBSan signature + top non-harness frame.
cd /home/x/c/tdesktop/dev/microtex-fuzz
BIN=$(pwd)/build_afl/afl_microtex
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1:handle_abort=2

classify() {
  f="$1"
  out=$(timeout 12 "$BIN" "$f" 2>&1)
  etype=$(printf '%s' "$out" | grep -oE "AddressSanitizer: [a-zA-Z0-9_-]+|runtime error: [^\n]+|stack-overflow|out-of-memory" | head -1)
  if [ -z "$etype" ]; then
    if [ -z "$out" ]; then etype="TIMEOUT_or_SILENT"; else etype="OTHER"; fi
  fi
  frame=$(printf '%s' "$out" | grep -oE "in tex::[A-Za-z0-9_:~]+" | grep -vE "runOne|toWide" | head -1)
  [ -z "$frame" ] && frame=$(printf '%s' "$out" | grep -oE "#[0-9]+ 0x[0-9a-f]+ in [A-Za-z0-9_:~]+" | head -1 | grep -oE "in [A-Za-z0-9_:~]+")
  sig=$(printf '%s|%s' "$etype" "$frame" | sed -E 's/0x0*[0-9a-f]+/0xADDR/g' | tr '\n' ' ')
  echo "$sig || $f"
}
export -f classify
export BIN ASAN_OPTIONS

ls triage/uniq/* | xargs -P 12 -I{} bash -c 'classify "{}"' > triage/summary.txt 2>/dev/null
echo "=== crash class histogram (count | signature) ==="
sed -E 's/ \|\| .*//' triage/summary.txt | sort | uniq -c | sort -rn
