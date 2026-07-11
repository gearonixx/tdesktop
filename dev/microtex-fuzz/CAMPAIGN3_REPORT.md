# MicroTeX Fuzzing — Campaign 3 Report (2026-07-03)

Defensive vulnerability research. Target: `tex::LaTeX::parse` as reached by the IV
markdown renderer (`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`),
i.e. attacker-controlled math in `$…$` / `$$…$$` spans of an IV article.

Companion docs: `CAMPAIGN2_REPORT.md` (N1/N2, F1), `SESSION_FINDINGS.md` (F1),
`findings/new/REPORT.md`.

---

## Setup — 2 fuzzers × 3 directions, 3h timebox

6 AFL++ 4.40c nodes against the persistent harness `build_afl/afl_microtex`
(ASan+UBSan, vptr off). Fresh `out3/` (campaign-2 `out/`/`out2/` untouched).

| Dir | Focus | Seeds | Strategy |
|-----|-------|-------|----------|
| A | broad havoc | `seeds/` (65) | `-p explore`, M+S |
| B | dictionary-guided (new `\command` paths) | `seeds/` + `tex.dict` | `-p coe -x tex.dict` |
| C | deep recursion + matrix/env/array | `seeds_struct/` (29) | `-p exploit -L 0` (MOpt) |

**~54M aggregate execs** (A ≈14.1M, B ≈9.9M, C ≈30.1M — MOpt/struct dir was
fastest). 1,511 unique-by-content crash files collected across all 6 nodes.

---

## Crash-class histogram (triaged against current tree via `afl_microtex_np`)

| Count | Signature | Status |
|------:|-----------|--------|
| 1438 | `TIMEOUT_or_SILENT` (hang/DoS bucket) | known, un-minimized |
| 61 | heap-buffer-overflow `ArrayFormula::insertAtomIntoCol` | **N2** (known) |
| 6 | global-buffer-overflow `SpaceAtom::getFactor` | **N3 — NEW** |
| 4 | SEGV `TeXParser::isValidCharInCmd` | **N1** (known) |
| 1 | SEGV `TeXParser::isValidName` | **N1** sibling site (known) |
| 1 | heap-buffer-overflow `MatrixAtom::createBox` | **N4 — NEW** |

Two new distinct live bugs (N3, N4). No RCE. Both DoS-class, reachable from IV math.

---

## N3 — `\kern` with missing length → `UnitType::none` (-1) OOB read *(NEW)*

**Class:** global-buffer-overflow (OOB read of a global `std::function` array, then
invokes it) → crash / DoS. Deterministic. **PoC (5 bytes): `\kern`**
(`findings/campaign3/pocN3_kern_getfactor.tex`).

**Path:** `SpaceAtom::getFactor` (`atom/atom_space.h:45`) ← `SpaceAtom::createBox`
(`atom_space.cpp:19`) ← `TeXRenderBuilder::build` ← `LaTeX::parse`.

**Root cause.** `TeXParser::getLength()` (`core/parser.cpp:694`) returns
`{UnitType::none, -1.f}` when the length argument is absent (`_pos == _len`).
`macro(kern)` (`macro_impl.cpp:12`) wraps that straight into a `SpaceAtom`. At box
build, `getFactor` does:
```cpp
return _unitConversions[static_cast<i8>(unit)](env);   // unit == none == -1
```
`UnitType::none == -1` (`utils/enums.h:124`), so this indexes `_unitConversions[-1]`
— one slot **before** the global array — reinterprets that memory as a
`std::function<float(const Environment&)>` and calls it. ASan faults in the
function's `_M_empty()` guard (READ of size 8). Fixed OOB offset (-1), not an
attacker-controlled index → deterministic crash, not a demonstrated write primitive.

**Why not RCE.** OOB is a constant −1 into a read-only global; no
attacker-controlled index or write. It faults deterministically. DoS.

**Proposed fix.** Guard the sink (mirror N1/N2 style), and/or reject `none` at the
`\kern` macro:
```cpp
inline static float getFactor(UnitType unit, const Environment& env) {
  const auto u = static_cast<i8>(unit);
  if (u < 0 || u >= _unitsCount) return 0.f;   // none / out-of-range → no space
  return _unitConversions[u](env);
}
```
(Same guard belongs in `getSize`.) Alternatively make `macro(kern)` treat a `none`
unit as `pixel`/zero-width.

---

## N4 — ragged-row OOB read in `MatrixAtom::createBox` (hline adjacency) *(NEW)*

**Class:** heap-buffer-overflow (READ of size 8, `sptr<Atom>`) → crash / DoS.
Deterministic. **PoC (25 bytes, minimized): `\begin{tabular}0\\&\hline`**
(`findings/campaign3/pocN4_min.tex`; original `pocN4_matrix_createbox.tex`).

**Crash site:** `MatrixAtom::createBox` (`atom/atom_matrix.cpp:603`) ← `Dummy` /
`RowAtom::createBox` ← `TeXRenderBuilder::build` ← `LaTeX::parse`.

**Root cause.** The `hline` branch checks the cell directly above for hline
adjacency:
```cpp
if (i >= 1 && dynamic_cast<HlineAtom*>(_matrix->_array[i - 1][j].get()) != nullptr)
```
`_array` rows are **ragged** (a short/empty row above a wider one). Column index `j`
is driven by the current (wider) row, so `_array[i-1][j]` indexes past the end of the
shorter previous row → OOB `vector::operator[]` read of an `sptr<Atom>`.

**Relation to N2.** Same *ragged-row* family, **different sink**: N2 is parse-time
(`insertAtomIntoCol`, `formula.cpp:214`); N4 is render-time (`createBox`,
`atom_matrix.cpp:603`). A per-row bound at N2 will not necessarily cover this read;
N4 needs its own bounds check.

**Why not RCE.** OOB read of a `sptr` during layout; no controlled write. DoS.

**Proposed fix (bound the column into the previous row):**
```cpp
auto& prev = _matrix->_array[i - 1];
if (i >= 1 && j < (int)prev.size()
    && dynamic_cast<HlineAtom*>(prev[j].get()) != nullptr) { ... }
```

---

## Hang bucket

1,438 `TIMEOUT_or_SILENT` (largest bucket, consistent with campaign 2). Deep
recursion / pathological loops in the parser; DoS-class, un-minimized. Next step:
`afl-tmin` representatives and split stack-overflow vs. infinite-loop.

## Repro cheatsheet
```
BIN=build_afl2/afl_microtex_np
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 $BIN findings/campaign3/pocN3_kern_getfactor.tex  # N3 global-overflow
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 $BIN findings/campaign3/pocN4_min.tex             # N4 heap-overflow
```
