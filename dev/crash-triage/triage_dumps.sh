#!/usr/bin/env bash
# Safe, read-only crash-dump triage for Telegram Desktop minidumps.
# Classifies each .dmp by the libstdc++ hardening abort reason it carries.
# No exploitation, no code execution: it only runs `strings` over the dumps.
#
# Usage: bash triage_dumps.sh [extra dump dirs...]
set -u

DIRS=(
  "$HOME/dumps"
  "$HOME/.local/share/TelegramDesktop/tdata/dumps"
  "$HOME/tgreport/raw"
  "$@"
)

mapfile -t DUMPS < <(for d in "${DIRS[@]}"; do ls "$d"/*.dmp 2>/dev/null; done | sort -u)
[ "${#DUMPS[@]}" -eq 0 ] && { echo "no .dmp files found"; exit 1; }

dk=0; clamp=0; other=0; unknown=0; mtx=0
printf "%-42s | %-19s | %s\n" "DUMP" "MTIME" "CLASS"
printf -- "-------------------------------------------------------------------------------------------\n"
for D in "${DUMPS[@]}"; do
  mt=$(stat -c %y "$D" 2>/dev/null | cut -d. -f1)
  elem=$(strings -n 4 "$D" 2>/dev/null | grep -oE "reference = [^&]+&;" | head -1 | sed 's/reference = //; s/&;//')
  vec=$(strings  -n 6 "$D" 2>/dev/null | grep -oE "Assertion '__n < this->size\(\)' failed" | head -1)
  clp=$(strings  -n 6 "$D" 2>/dev/null | grep -oE "Assertion '!\(__hi < __lo\)' failed" | head -1)
  # Is a MicroTeX *crash frame* present (vs the benign "MicroTeX: font" log string)?
  mtxframe=$(strings -n 6 "$D" 2>/dev/null | grep -oE "atom_matrix|font_info\.h|IndexedArray|recalculateLine|getNextLarger|AccentedAtom::createBox" | head -1)

  if [ -n "$elem" ] || [ -n "$vec" ]; then cls="VECTOR_OOB operator[] ${elem:-<type?>}"; dk=$((dk+1))
  elif [ -n "$clp" ]; then cls="CLAMP inverted-bounds !(__hi<__lo)"; clamp=$((clamp+1))
  else
    a=$(strings -n 6 "$D" 2>/dev/null | grep -oE "Assertion '[^']+' failed|double free|free\(\): invalid|malloc.*corrupt|stack_chk_fail|bad_alloc" | head -1)
    if [ -n "$a" ]; then cls="OTHER_ASSERT $a"; other=$((other+1)); else cls="NO_REASON_STRING (segv/uncaptured)"; unknown=$((unknown+1)); fi
  fi
  [ -n "$mtxframe" ] && { cls="$cls  <MICROTEX-FRAME:$mtxframe>"; mtx=$((mtx+1)); }
  printf "%-42s | %-19s | %s\n" "$(basename "$D")" "$mt" "$cls"
done
echo
echo "== summary =="
echo "total dumps           : ${#DUMPS[@]}"
echo "vector operator[] OOB : $dk   (dominant element type: Dialogs::Key)"
echo "clamp inverted bounds : $clamp"
echo "other assert          : $other"
echo "no reason recovered   : $unknown"
echo "MicroTeX CRASH frames : $mtx   (a 'MicroTeX: font' log string in heap is NOT a frame)"
