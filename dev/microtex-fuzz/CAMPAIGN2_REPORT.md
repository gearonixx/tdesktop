# MicroTeX Fuzzing — Campaign 2 Report & Crash Triage (2026-07-02)

Defensive vulnerability research. Target: `tex::LaTeX::parse` as reached by the IV
markdown renderer (`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`),
i.e. attacker-controlled math in `$…$` / `$$…$$` spans of an IV article.

Companion docs (do not delete): `SESSION_FINDINGS.md` (campaign 1, F1),
`findings/new/REPORT.md` (N1/N2 detail), `another_finding.md` (F-OPEN-1 disproof).

---

## TL;DR

- 9 AFL++ nodes, ~4h43m, **~50M aggregate executions**, saved **2,807 (out) + 427
  (out2)** crash files.
- That raw count is coverage-path inflation. After dedup + re-triage **against the
  current, already-patched tree**: **2 distinct live memory-safety bugs**, plus a
  large hang/DoS bucket. Everything else was already-fixed (F1/F2/F3) or persistent-
  mode state artifacts.
- **No RCE.** Both live bugs are client-side **crash / DoS**. The lone "RCE
  candidate" from campaign 1 (F-OPEN-1) is a **fuzzer harness artifact**, not a
  MicroTeX defect.
- Fuzzers **stopped** 2026-07-02 (coverage plateaued: `pending_favs: 0`, main node
  idle ~1h40m before stop).

---

## Campaign statistics (at stop)

| Node | run_time | execs | exec/s | cycles | corpus | saved_crashes | saved_hangs |
|------|---------:|------:|-------:|-------:|-------:|--------------:|------------:|
| out/m0 (main) | 16972 | 9.76M | 575 | 36 | 7212 | 538 | 166 |
| out/s1 | 16973 | 6.43M | 379 | 3 | 6388 | 511 | 155 |
| out/s2 | 16974 | 6.62M | 390 | 3 | 6873 | 506 | 154 |
| out/s3 | 16974 | 6.73M | 396 | 3 | 6836 | 511 | 163 |
| out/s4 | 16973 | 5.41M | 319 | 2 | 6248 | 472 | 160 |
| out/s5 | 16974 | 6.60M | 389 | 2 | 6765 | 493 | 152 |
| out/s6 | 16973 | 8.72M | 514 | 3 | 7613 | 620 | 166 |
| out2/p0 (dict) | 16269 | 2.12M | 130 | 5 | 5989 | 169 | 3 |
| out2/p1 (dict) | 16244 | 2.10M | 129 | 0 | 6070 | 258 | 2 |

Aggregate crash files: 2,807 (out) + 427 (out2). Unique-by-content (out): 2,805.

---

## What the crash pile actually is

Bucketed through the ASan/UBSan build (`build_afl/afl_microtex_np`,
`build_asan/afl_np_asan`); histograms in `triage/summary.txt`,
`triage/summary_np.txt`, `triage/allsummary.txt`.

| Bucket | Raw magnitude | Reality | Vs. current tree |
|--------|--------------|---------|------------------|
| `TIMEOUT_or_SILENT` (hangs) | ~622–624 / node | deep recursion / infinite loop | **live DoS bucket, un-minimized** |
| `IndexedArray` OOB (F1) | 622 (np corpus) | one bug, mass-duplicated | **FIXED** (verified exit 0) |
| ctype-on-`wchar_t` SEGV | few | **N1** | **LIVE** ✓ |
| `insertAtomIntoCol` heap-overflow | few | **N2** | **LIVE** ✓ |
| everything else | ~426 of 428 uniq | F1/F2/F3 fixed + persistent-mode state | gone |

Re-verified at stop time: N1 → `SEGV`, N2 → `heap-buffer-overflow`,
F1 PoC (`findings/bug1_accent_oob/poc_min_1byte.bin`) → clean exit 0.

---

## Findings table

| ID | Class | Crash site | PoC | Severity | RCE? |
|----|-------|-----------|-----|----------|------|
| **N1** | OOB read (wild) → SIGSEGV | `isalpha` ← `isValidCharInCmd` `parser.h:354` ← `getGroup` ← `inflateEnv` | `\begin{env}…` + astral codepoint (99 B) | Medium — DoS | **No** |
| **N2** | heap-buffer-overflow | `ArrayFormula::insertAtomIntoCol` `formula.cpp:214` ← `MatrixAtom::parsePositions` `atom_matrix.cpp:82` | `\begin{cases}\\&` (16 B) | Medium/High — DoS, potential heap corruption | **No demonstrated** |
| F1 | global-buffer-overflow (OOB read) | `IndexedArray::compare` ← `FontInfo::getNextLarger` ← `AccentedAtom::createBox` | any accented Latin-1 char, `$é$` | (was) Medium — DoS | No — **FIXED** |
| Hangs | resource exhaustion | parser recursion / loop | ~622/node, un-minimized | Low/Medium — DoS | No |
| F-OPEN-1 | SEGV `@0x0d` under `-fsanitize=fuzzer` only | `__dynamic_cast` on Atom RTTI | `\substack{a\\b}`, `\color{red}{x}` | **Not a bug** | **No** |

---

## Finding N1 — raw `wchar_t` into C `<cctype>` (systemic)

**Class:** OOB read → SIGSEGV / DoS. Value-dependent.
**Confirmed path:** `isalpha` ← `TeXParser::isValidCharInCmd` (`core/parser.h:354`)
← `getGroup` (`core/parser.cpp:236`) ← `inflateEnv` (`:798`) ← `preprocess`
(`:822`) ← `Formula::setLaTeX` ← `LaTeX::parse`.

**Root cause.** `isValidCharInCmd(wchar_t ch)` calls `isalpha(ch)` on an
unconverted `wchar_t`. C `isalpha`/`islower`/… are defined only for values
representable as `unsigned char` or `EOF`; anything else is UB. glibc indexes
`__ctype_b_loc()` by the raw value, so a large `ch` (astral-plane codepoint,
observed index ≈ 0x71C71) reads far outside the table and faults on an unmapped
page. Smaller values silently return garbage — still UB, and it makes
command/group scanning nondeterministic on non-ASCII input.

**Systemic — same pattern at 7 sites:** `parser.h:354`; `parser.cpp:507`, `:512`
(`isValidName`, every `\command`); `macro_impl.cpp:443`; `atom_space.cpp:32`;
`atom_char.cpp:31/81/100`; `atom_row.cpp:206`.

**Why not RCE.** Read-only wild access; no attacker-controlled write, no corrupted
pointer followed for control flow. It crashes; it does not hijack.

**Proposed fix (all sites).** ASCII-guarded helpers in `utils/utils.h`:
```cpp
inline bool tex_isalpha(wchar_t c){ return c>=0 && c<128 && std::isalpha((int)c); }
inline bool tex_islower(wchar_t c){ return c>=0 && c<128 && std::islower((int)c); }
inline bool tex_isdigit(wchar_t c){ return c>=0 && c<128 && std::isdigit((int)c); }
inline wchar_t tex_toupper(wchar_t c){ return (c>=0 && c<128)?(wchar_t)std::toupper((int)c):c; }
```
Route the 7 sites through them (ASCII-only is correct: every command name / length
keyword / TeX letter these gate on is ASCII).

**Repro:** `ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 build_afl/afl_microtex_np
findings/new/pocA_isalpha_segv.tex`. Full trace: `findings/new/pocA_asan.txt`.

---

## Finding N2 — `insertAtomIntoCol` heap-overflow on ragged rows

**Class:** heap-buffer-overflow (ASan-confirmed READ; bad insert index can also
write past a short row) → crash / heap corruption. Deterministic.
**Crash site:** `ArrayFormula::insertAtomIntoCol` (`core/formula.cpp:214`)
← `MatrixAtom::parsePositions` (`atom/atom_matrix.cpp:82`) ← `MatrixAtom` ctor
← `macro_arrayATATenv` (`macro_impl.h:589`).

**Root cause.** The `@{…}` column-spec clamp uses `cols()` = the **maximum**
column count across rows (`atom_matrix.cpp:81`), then inserts at that index into
**every** row. Array rows are ragged, so for a short row `col > _array[j].size()`,
and `_array[j].insert(it + col, atom)` uses an iterator past `end()` — UB; the
realloc relocation reads/writes out of bounds.

**Why not (demonstrated) RCE.** ASan reproduces a `READ of size 8` during
`vector::_M_realloc_insert` relocation → crash. A write-past-end is theoretically
reachable, but `col` is bounded by the formula's own small `cols()` (not a large
attacker integer) and no controlled-write PoC was produced in ~50M execs. Treat as
DoS with heap-corruption risk; do not label RCE without a write primitive.

**Proposed fix (per-row clamp at sink):**
```cpp
void ArrayFormula::insertAtomIntoCol(int col, const sptr<Atom>& atom) {
  _col++;
  for (size_t j = 0; j < _row; j++) {
    auto& row = _array[j];
    const size_t at = std::min((size_t)std::max(col, 0), row.size());
    row.insert(row.begin() + at, atom);
  }
}
```

**Repro:** same harness, `findings/new/pocB_insertcol_oob.tex` (16 B).
Full trace: `findings/new/pocB_asan.txt`.

---

## Hang bucket — un-minimized DoS

~622 inputs/node classify as `TIMEOUT_or_SILENT`: almost certainly deep-recursion
stack overflow or pathological/infinite loops in the parser. DoS-class only (a
crafted math span hangs/overflows the stack on render). Not yet minimized to a root
cause — this is the largest remaining unknown-severity item. Next step: `afl-tmin`
each representative and separate stack-overflow from infinite-loop.

---

## F-OPEN-1 — the "RCE candidate" is a harness artifact (NOT a bug)

`\substack{a\\b}` and `\color{red}{x}` SEGV `@0x0d` **only** when the harness is
linked with `-fsanitize=fuzzer`. Proof (same MicroTeX lib, same inputs):

| build | `\substack{a\\b}` | `\color{red}{x}` |
|-------|-------------------|------------------|
| argv harness (ASan+UBSan) | exit 0 | exit 0 |
| AFL non-persistent (ASan+UBSan) | exit 0 | exit 0 |
| harness_fuzz (ASan+UBSan+fuzzer) | SEGV 0x0d | SEGV 0x0d |
| harness_fuzz (ASan-only+fuzzer) | SEGV 0x0d | SEGV 0x0d |

`-fsanitize=fuzzer` links a second copy of C++ RTTI that diverges from libstdc++'s;
`dynamic_cast` over the multiply-inherited Atom hierarchy walks foreign `type_info`
→ SEGV on valid objects. Removing UBSan changes nothing (not a vptr issue). Zero
product impact. Full disproof: `another_finding.md`.

---

## RCE assessment (opinion)

**Nothing found is RCE-level.** N1 is a read fault (crash, no write, no
control-flow primitive). N2's demonstrated behavior is an OOB read crash; its
theoretical write angle is unproven and bounded by a small internal index. F1 is
fixed and was an OOB read. Hangs are DoS by nature. F-OPEN-1 is not a bug. The two
live bugs are real, reachable from attacker-controlled IV math, and worth patching
— but they are **denial-of-service**, not remote code execution.

---

## Recommendations

1. **Fuzzers stopped** — done; coverage had plateaued.
2. **Land N1 + N2 fixes** — both live, both small diffs, both reachable from IV math.
3. **Minimize the hang bucket** with `afl-tmin`; classify stack-overflow vs. loop.
4. **Fix the harness** — do not link `-fsanitize=fuzzer` for the RTTI-heavy build,
   so it stops manufacturing phantom RCE crashes (F-OPEN-1).

## Repro cheatsheet

```
BIN=build_afl/afl_microtex_np
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 $BIN findings/new/pocA_isalpha_segv.tex   # N1 SEGV
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 $BIN findings/new/pocB_insertcol_oob.tex  # N2 heap-overflow
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 $BIN findings/bug1_accent_oob/poc_min_1byte.bin  # F1 (fixed → exit 0)
```
