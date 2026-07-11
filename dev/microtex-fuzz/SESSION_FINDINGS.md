# MicroTeX / IV Markdown Fuzzing — Session Findings (defensive vuln research)

Date: 2026-07-02. Branch: overshoot-fix. Target: Telegram Desktop, modules
**MicroTeX (LaTeX engine)** and **IV markdown stack** — the two new areas that
must be clean before the next release.

All work lives in `/home/x/c/tdesktop/dev/microtex-fuzz/`. Nothing in the real
source tree was modified (another agent runs in parallel; keep edits inside
`dev/microtex-fuzz/` and its `override/` include dir only).

---

## TL;DR — what was found

**CONFIRMED memory-safety bug #1 (real, deterministic, 1-byte PoC):**
`global-buffer-overflow` (OOB read) in MicroTeX's font "next larger" lookup,
reached while laying out an **AccentedAtom**. Triggered by ANY precomposed
accented Latin-1 letter (À Á Â … é … ÿ, 51 codepoints) placed in a math span.

- Crash site: `tex::IndexedArray<int,3,1>::compare` (`src/utils/indexed_arr.h:19`)
  via `FontInfo::getNextLarger` (`src/fonts/font_info.h:128`)
  via `DefaultTeXFont::hasNextLarger` (`src/fonts/fonts.h:213`)
  via `AccentedAtom::createBox` (`src/atom/atom_basic.cpp:335`).
- Root defect: `FontInfo::__get(int id) { return _infos[id]; }`
  (`src/fonts/font_info.h:74`) has **no bounds check**; `getInfo()` /
  `getNextLarger` / `hasNextLarger` never validate the Char's font code. A Char
  produced in the accent next-larger chain ends up with an out-of-range/garbage
  `_fontCode` (observed 21845 while `_infos.size()==35`), so `_infos[id]` reads
  the vector out of bounds and returns a **garbage `FontInfo*`** whose garbage
  `_nextLargers` (`_raw` wild, `_rows` huge) makes the binary search read wild
  memory. Layout-dependent: the OOB target is sometimes cmr10's global largers
  table (global-buffer-overflow) and sometimes a heap address.
- 100% reproducible in the release-like `-O1` build. Reachable through the real
  IV path (works with `\displaystyle ` prefix and inside `$$...$$`).
- Reachability in product: IV markdown renders `$...$` / `$$...$$` math via
  `MeasureWithMicrotex`/`RenderWithMicrotex` → `tex::LaTeX::parse`
  (`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`). A markdown
  article containing a math formula with an accented letter (e.g. `$é$`)
  triggers it on render — minimal interaction (open the article).

PoCs saved in `findings/bug1_accent_oob/`:
- `poc_min_1byte.bin` = single byte `0xFF`.
- `poc_utf8_yuml.txt` = UTF-8 `ÿ` (C3 BF) — how it appears in a real .md file.
- `poc_math_eacute.txt` = `$$é$$` (UTF-8).
- `asan_report.txt` = full ASan output.

Trigger codepoints (single char each): U+00C0..C5, C8..CF, D1..D6, D9..DD,
E0..E5, E8..EF, F1..F6, F9..FD, FF (all precomposed accented Latin letters that
MicroTeX decomposes into base+accent → AccentedAtom).

NOTE: `src/utils/indexed_arr.h` already carries a comment about a *previous*
IndexedArray OOB fix (inclusive-high binary search, `h=_rows-1`). That fix
assumes `_raw`/`_rows` are valid; it does NOT help when the whole `FontInfo`
(hence the whole IndexedArray) is garbage from an OOB `_infos[id]`. So the
proper fix is bounds-checking `FontInfo::__get` and validating font codes in
`getInfo`/`getNextLarger`/`hasNextLarger`, plus guarding the unchecked
`(SymbolAtom*)_accent.get()` cast at `atom_basic.cpp:333`.

STATUS: root-caused, PoC'd. Not yet: exact upstream reason the chain yields a
bad font code (largers-chain over accent glyph produces an entry whose
larger-font-id / char is invalid, or an uninitialized Char in the chain —
memory read shows ASan poison 0xbebebe, i.e. uninitialized/OOB font memory in
the getNextLarger chain). Fix at the defensive choke points regardless.

---

## Harness / infrastructure that WORKS (reuse this)

Key discovery that makes headless fuzzing possible: **MicroTeX font metrics are
compiled in** (`src/res/reg/builtin_font_reg.h`, `builtin_syms_reg.h`, and
`src/res/font/*.def.cpp`). So parse + layout (createBox, which computes box
sizes from metrics) run WITHOUT Qt / without glyph rasterization. Only two
platform factories must be stubbed (they're only used for drawing/measuring
text, which we don't exercise): `Font::create` and `TextLayout::create`.

Files (all in `dev/microtex-fuzz/`):
- `stub_platform.cpp` — StubFont / StubTextLayout implementing the 2 factories.
- `fuzz_microtex.cpp` — libFuzzer harness (calls `LaTeX::initBundled()` then
  `LaTeX::parse`). **libFuzzer runtime has an artifact — see gotchas.**
- `afl_microtex.cpp` — AFL++ **persistent** harness (`__AFL_LOOP`). Crashes here
  are STATE-dependent across iterations and do NOT reproduce as single inputs.
- `afl_microtex_np.cpp` — AFL++ **non-persistent** (deferred forkserver, one
  parse per fork; also a file-arg fallback for triage). **Use this one** — every
  crash is a deterministic single-input repro. This is the primary harness.
- `filemain.cpp` — plain non-AFL file-reading main (for gdb/manual repro).
- `build.sh` — builds the libFuzzer binary.
- `build_afl.sh` — builds the AFL++ persistent binary (`AFL_USE_ASAN/UBSAN`).
- `tex.dict` — LaTeX dictionary. `seeds/` — 51 seed formulas.
- `triage.sh` — parallel crash classifier (bucket by ASan sig + top frame).
- `override/utils/indexed_arr.h` — instrumented copy (debug only).

### Build commands
```
cd /home/x/c/tdesktop/dev/microtex-fuzz
bash build.sh          # libFuzzer  -> build/fuzz_microtex
bash build_afl.sh      # AFL persistent -> build_afl/afl_microtex
# non-persistent (primary), reuse lib objs from build_afl/obj:
MT=/home/x/c/tdesktop/Telegram/ThirdParty/MicroTeX/src
FL="-std=c++17 -g -O1 -fno-omit-frame-pointer -fno-sanitize=vptr -I$MT -DNDEBUG"
AFL_USE_ASAN=1 AFL_USE_UBSAN=1 afl-clang-fast++ $FL -c afl_microtex_np.cpp -o build_afl/obj_np.o
PERS=build_afl/obj/$(echo /home/x/c/tdesktop/dev/microtex-fuzz/afl_microtex.cpp|md5sum|cut -c1-16).o
AFL_USE_ASAN=1 AFL_USE_UBSAN=1 afl-clang-fast++ $FL $(ls build_afl/obj/*.o|grep -v "$(basename $PERS)") build_afl/obj_np.o -o build_afl/afl_microtex_np
```

### Run AFL (system core_pattern is a pipe here)
```
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_SKIP_CPUFREQ=1 AFL_NO_AFFINITY=1
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=0   # symbolize=0 REQUIRED by AFL
export AFL_FORKSRV_INIT_TMOUT=60000
afl-fuzz -i seeds -o out -x tex.dict -m none -M m0 -- ./build_afl/afl_microtex_np   # + -S s1..sN secondaries
```

### Triage a single crash file
```
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1 ./build_afl/afl_microtex_np <file>
```

---

## CRITICAL gotchas (do not re-derive — cost hours this session)

1. **UBSan `vptr` = false positives.** With `-fsanitize=address,undefined`, the
   `vptr` check crashes inside libstdc++ `__dynamic_cast` for MicroTeX's
   multiply-inherited atoms (e.g. `class ColorAtom : public Atom, public Row`)
   even on VALID objects (SEGV at addr 0x0d in `Formula::add`'s
   `dynamic_pointer_cast<MiddleAtom>`). Verified NOT a real bug (no repro without
   sanitizers; not in the real Telegram build). **Always build with
   `-fno-sanitize=vptr`.** Symptom if you forget: every `\color`, `\textcolor`,
   and unknown-command input "crashes."

2. **libFuzzer runtime artifact.** The SAME code, same library, driven by our own
   `main`, SURVIVES; under the libFuzzer runtime `\color{red}{x}` SEGVs at 0x0d.
   It's a libFuzzer-runtime interaction, not a real bug. => prefer AFL++.

3. **Persistent mode (`__AFL_LOOP`) crashes are state-dependent** and won't
   reproduce as single inputs (SIGABRT `sig:06`). Use the **non-persistent**
   harness for clean, deterministic per-input PoCs. The real app resets global
   state per parse (`LaTeX::parse` calls `NewCommandMacro::_reset()`,
   `MatrixAtom::resetState()`, `RowAtom::_breakEveywhere=false`), so
   single-input repros are the meaningful ones.

4. AFL requires `ASAN_OPTIONS=...:symbolize=0` or it aborts at startup.

5. The global `LaTeX` formula parser runs in **partial mode** (`_isPartial=true`)
   — unknown commands don't throw, they render a red placeholder
   (`parser.cpp:532`). So malformed LaTeX is tolerated, widening the surface.

6. `-O0`/gdb is unreliable for THIS bug (heisenbug: hardware watchpoint or any
   inserted code makes it vanish → uninitialized/layout-dependent). Trust the
   `-O1` AFL/ASan report as authoritative.

---

## Fuzzing results so far

- First AFL campaign (7 instances, persistent build, ~7 min): 302 "crashes" +
  268 "hangs" saved, but those were persistent-mode state artifacts.
- Re-triaged 624 unique crash inputs against the **non-persistent** binary:
  **622/624 → `global-buffer-overflow in tex::IndexedArray` (Bug #1)**, 2 clean.
  => one dominant bug funnel. Other bugs likely masked by it.
- Hangs (268) not yet triaged — likely stack-overflow (deep recursion) DoS
  class, which is lower value (client DoS, already known category). TODO.

`out/` contains the AFL sync dir (corpus + crashes). `triage/uniq/` has the 624
deduped crash inputs. `triage/summary_np.txt` has the per-input classification.

---

## NEXT STEPS (continue here)

1. **Harden the harness to fuzz PAST Bug #1** so other bugs surface. Create
   `override/fonts/font_info.h` with a bounds-checked `__get` (throw/normalize on
   OOB id) and rebuild the AFL harness with `-Ioverride` BEFORE `-I$MT`. Then
   re-fuzz; new crash signatures = new bugs. (Do NOT edit real source.)
2. Triage the 268 hangs (afl-tmin + check for stack-overflow vs infinite loop).
3. Manually chase the leads from prior agents that the fuzzer hasn't stressed:
   - unchecked atom downcasts: `atom_basic.cpp:333` `(SymbolAtom*)_accent.get()`
     (in Bug #1 path), `:611` `static_cast<SideSetsAtom*>`, `atom_row.cpp:21/31/33`.
   - array desync: `(ArrayFormula*)_formula` casts guarded by `_arrayMode`
     (`parser.cpp:910/947`, `macro_impl.h:73/108/566/580`) — find a path leaving
     `_arrayMode==true` while `_formula` is a plain Formula.
   - `createBox` recursion depth vs the `kMaxParseDepth=250` transitive bound.
4. **Second target — cmark-gfm / IV markdown C parser** (not yet started):
   `Telegram/ThirdParty/cmark-gfm/src/{inlines,blocks,scanners,houdini_*}.c` and
   `extensions/table.c`. Upstream harness exists: `cmark-gfm/test/cmark-fuzz.c`.
   Build a libFuzzer/AFL harness around `cmark_parse_document` with the GFM
   extensions enabled (tables/strikethrough/autolink/tasklist/tagfilter), since
   IV enables them. Also fuzz the IV-specific glue
   `iv_markdown_parse_convert.cpp` / `iv_markdown_parse_validate.cpp`.

## Key file:line references (verified this session)
- `iv_markdown_microtex.cpp:280` — `tex::LaTeX::parse(...)` handoff.
- `latex.cpp:163-188` — `LaTeX::parse` (resets state, build).
- `parser.cpp:507-534` — `processEscape` (partial-mode red placeholder at 532).
- `atom_basic.cpp:277-294` — `AccentedAtom(base,name)` ctor (validates _accent).
- `atom_basic.cpp:316-355` — `AccentedAtom::createBox` (the vulnerable loop @335).
- `fonts.cpp:259-263` — `DefaultTeXFont::getNextLarger` (double `getInfo`, no bounds).
- `fonts.h:212-214` — `DefaultTeXFont::hasNextLarger` (no fontCode validation).
- `font_info.h:74` — `FontInfo::__get(id){return _infos[id];}` (**no bounds check**).
- `font_info.h:127-131` — `FontInfo::getNextLarger` (returns CharFont(item[1],item[2])).
- `indexed_arr.h:17-65` — `IndexedArray::compare` / `operator()` (binary search).
