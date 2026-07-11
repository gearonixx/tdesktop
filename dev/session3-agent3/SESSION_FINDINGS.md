# MicroTeX / IV Markdown robustness review — Session 3 (agent 3)

Date: 2026-07-02. Branch: `overshoot-fix`. Targets: **MicroTeX (LaTeX engine)**
and the **IV markdown stack** — the two newly-added parsing surfaces that must be
memory-safe before the next release.

This continues two prior sessions. Their state:
- `setup/FINDINGS.md` — F1–F4 fixed in-tree (matrix `\multirow` OOB, IndexedArray
  off-by-one, arg-collection stack overflow, alloc/dealloc mismatch); F-OPEN-1
  ruled a libFuzzer false positive.
- `dev/microtex-fuzz/SESSION_FINDINGS.md` — the AFL campaign that found the accent
  OOB (its "Bug #1") and the harness infrastructure.

Session-3 goal: (a) independently *verify* the prior verdicts against the current
tree, (b) resolve the one real disagreement between the two prior sessions, and
(c) fuzz/audit *past* the dominant bug funnel to surface anything still hiding.

Work lives in `dev/session3-agent3/`. The real source tree already carries F1–F4
(see `git -C Telegram/ThirdParty/MicroTeX diff`); nothing new patched yet this
session.

Harness note: the app's live path is
`iv_markdown_microtex.cpp:ParseFormula → tex::LaTeX::parse` (full parse + box
build). All harnesses drive exactly that, with fonts/metrics compiled in from
`res/**/*.def.cpp` and the two platform factories stubbed (`graphic_stub.cpp`).

================================================================================
## Iteration 1 — verify prior verdicts; settle the session disagreement
================================================================================

### 1a. The two prior sessions disagreed on ONE crash. Resolved: same bug.

Both sessions hit the identical ASan stack:

    IndexedArray::compare ← operator() ← FontInfo::getNextLarger
      ← DefaultTeXFont::hasNextLarger ← AccentedAtom::createBox (atom_basic.cpp:335)

- `setup/FINDINGS.md` called it **F2**: an inclusive-high binary-search off-by-one
  in `IndexedArray::operator()` (`h=_rows` lets `m` reach `_rows`, so `_raw+m*N`
  reads one row past the largers table). Fixed with `h=_rows-1`.
- `dev/.../SESSION_FINDINGS.md` called it a **distinct** "Bug #1": an unchecked
  `FontInfo::__get(id){return _infos[id];}` reached by a *garbage font code*
  (observed 21845 / `0xbebebe`), and explicitly claimed F2's fix does **not**
  cover it.

**Resolution (this session): they are the same bug, and F2 fixes it.** The crash
site is the binary search inside `_nextLargers((int)ch)` itself — i.e. the *first*
lookup — not a downstream `_infos[garbage]`. The "garbage font code 21845/0xbebebe"
the dev session saw is precisely the bytes the off-by-one read one row past the
table; it never got as far as `_infos[id]` with that value on a fixed binary
search. Verified empirically below.

### 1b. Empirical verification (argv harness, current tree, ASan+UBSan `-O1`)

Rebuilt `setup/fuzzers/microtex/build/harness` against the current (F1–F4) source.
`ASAN_OPTIONS=abort_on_error=1`. Each input parsed the same way the app parses a
`$...$` span.

| input                       | single-parse | 7-in-one-process |
|-----------------------------|:------------:|:----------------:|
| `\hat{x}`                   | exit 0       | exit 0           |
| `\displaystyle \hat{x}`     | exit 0       | exit 0           |
| `$$é$$` (U+00E9, valid UTF8)| exit 0       | exit 0           |
| `ÿ` (U+00FF)                | exit 0       | exit 0           |
| `0xFF` (raw byte)           | exit 0       | exit 0           |
| `\substack{a\\b}`           | exit 0       | exit 0           |
| `\color{red}{x}`            | exit 0       | exit 0           |
| `\left(x\middle|y\right)`   | exit 0       | exit 0           |

- Accent path (`\hat`, `$$é$$`, `ÿ`): **clean → F2 fixed it.** The dev session's
  "second unchecked-`_infos[id]` bug" was a mis-attribution of the same off-by-one.
- The multi-parse column matters: the app parses up to 10 000 formulas per process
  on worker threads, so cross-parse global-state contamination would be a *real*
  bug. 7 diverse formulas (incl. the disputed ones) in one process: still clean.

### 1c. F-OPEN-1 (`\substack`/`\color`/`\middle` SEGV @0x0d): confirmed a
`-fsanitize=fuzzer` toolchain artifact — NOT a MicroTeX bug, NOT RCE.

Built a properly coverage-instrumented libFuzzer target from the *same* MicroTeX
sources and *same* `-fsanitize=address,undefined` (vptr enabled). Result:

| build (identical MicroTeX .o, identical ASan+UBSan)        | `\substack` | `\color` | `\middle` | `é`/`\hat` |
|------------------------------------------------------------|:-----------:|:--------:|:---------:|:----------:|
| argv harness (no `-fsanitize=fuzzer`)                      | exit 0      | exit 0   | exit 0    | exit 0     |
| libFuzzer harness (`+ -fsanitize=fuzzer`)                  | SEGV 0x0d   | SEGV 0x0d| SEGV 0x0d | exit 0     |

Backtrace of the libFuzzer crash:

    #0 libstdc++.so.6+0xb885f
    #1 __dynamic_cast            (STATIC copy inside harness_fuzz)  ← second RTTI
    #2 dynamic_pointer_cast<MiddleAtom>
    #3 Formula::add (formula.cpp:110)
    #4 TeXParser::parse (parser.cpp:926)

Mechanism (now proven, not hypothesised): `-fsanitize=fuzzer` statically links a
second `__dynamic_cast`/RTTI into the binary; it calls the system `libstdc++.so.6`
type-info walker with a mismatched `type_info` representation and faults reading
`0x0d`. It fires **only for multiply-inherited atoms** (`ColorAtom : public Atom,
public Row`; the `\middle`/`\substack` products) whose downcast takes the
sub-object-traversal path — which is why single-inheritance `\hat`/`é` never trip
it. The argv build resolves one `__dynamic_cast` (system libstdc++ only) and is
clean on all inputs.

**Consequences:**
1. F-OPEN-1 is a false positive. `Formula::add`'s `dynamic_pointer_cast` is correct
   C++ and works in the shipped client (which links no libFuzzer runtime).
2. **libFuzzer is unusable for this target** — it false-crashes on essentially any
   formula reaching a multiply-inherited-atom downcast. All coverage-guided fuzzing
   this session uses **AFL++ non-persistent** (afl-clang-fast, no second RTTI),
   whose crashes are deterministic single-input repros.

Status after iteration 1: prior fixes hold; the one open disagreement is resolved;
methodology corrected. Next: fuzz past the F2 funnel with AFL, and static-audit the
matrix/array index math and the IV-markdown glue (our newest code).

================================================================================
## Iteration 2 — AFL past the F2 funnel: TWO NEW reachable null-deref crashes
================================================================================

Launched AFL++ non-persistent (3 instances, ASan+UBSan, `-O1`) on the current
(F1–F4) tree, seeded with 82 formulas incl. matrix/array/dimension/script/
left-right constructs. Within ~15 s and <15k execs each instance saved a crash.
Triaged against the argv ASan harness (deterministic single-input). Two **new,
distinct, deterministic** signatures — neither is one of F1–F4, and both are
genuine **NULL-pointer dereferences during the box-build (layout) phase**:

### N1 — null-base `MathAtom` → SIGSEGV in `MathAtom::createBox`  [DoS]
- Site: `src/atom/atom_basic.cpp:35` — `auto box = _base->createBox(e);` with
  `_base == nullptr`.
- Origin: `src/core/parser.cpp:904` builds
  `sptrOf<MathAtom>(Formula(*this, getDollarGroup(DOLLAR), false)._root, style)`.
  When the `$…$` group parses to **no root atom** (empty / env-opener-only body
  such as `\[`), `_root` is null, so the `MathAtom`'s `_base` is null and its
  `createBox` dereferences null.
- Repro (as parsed by the app): `\displaystyle $\[$`  (also `$$\($$`, `$\[$`).
- Class: `MathAtom` has **no** null-base guard, unlike `AccentedAtom::createBox`
  (`:398`) and the `OverUnderBox` path (`:845`) which both already do
  `_base == nullptr ? StrutBox : _base->createBox(...)`.

### N2 — null-base `UnderOverAtom` → SIGSEGV in `UnderOverAtom::leftType()`  [DoS]
- Site: `src/atom/atom_basic.h:528` (and `:532` `rightType`) —
  `return _base->leftType();` with `_base == nullptr`. Reached via
  `RowAtom::changeToOrd` → `Dummy::leftType` (`atom_row.cpp:102`) during
  `RowAtom::createBox`, so it only fires when the null-base atom has a **sibling**
  in a row (a lone one never calls `changeToOrd`).
- Origin: `macro(underaccent)` (`src/core/macro_impl.h:793`) builds
  `sptrOf<UnderOverAtom>(Formula(tp, args[2], false)._root, …)`. `\b` expands to
  `\underaccent{\bar}{#1}` (`macro_def.cpp:397`); with `#1` = a lone `$`, the base
  sub-formula is parsed in math mode where a bare `$` yields no atom → null
  `_root` → null `_base`.
- Repro (as parsed by the app): `\displaystyle \b$x$y`.
- **End-to-end reachability CONFIRMED** through the IV path: markdown
  `$$\b$x$y$$` — the display-math scanner (`iv_markdown_math.cpp:328-352`) only
  terminates on `$$`, so lone `$` survives into the extracted content, giving
  `trimmedTex = \b$x$y`; `PreparedTeX` prepends `\displaystyle `. Pure ASCII, no
  NUL → passes `ValidateMarkdownSourceForIv` (valid-UTF-8/no-NUL/<10%-control gate).

### Severity (both N1, N2): client-side DoS, NOT memory corruption / NOT RCE.
Null-pointer virtual calls fault at a low address (guard page) → clean SIGSEGV.
The app wraps `LaTeX::parse` in `try/catch` (iv_markdown_microtex.cpp:279) but a
**signal is not a C++ exception**, so the crash is not caught — it takes down the
client process. A single malicious IV markdown article (opened by the victim)
containing one display-math span crashes the client on render. Same severity class
as F3 (remote client DoS via crafted formula), distinct trigger.

### Root pattern: a *class* of missing null-base guards.
`Formula(tp, args[N], false)._root` (and the dollar-group root at parser.cpp:904)
can legitimately be null, and several atoms deref `_base` with no guard. Survey of
`_base->` sites shows the guard exists in some atoms and is missing in others:

    GUARDED already : atom_basic.cpp:398 (Accented), :845 (OverUnder box),
                      atom_basic.h:565/569 (leftType/rightType null-checked)
    UNGUARDED       : atom_basic.cpp:35 (MathAtom::createBox)      <- N1
                      atom_basic.h:528/532 (UnderOverAtom l/r Type) <- N2
                      atom_basic.h:120/122, atom_basic.cpp:22/238/453… (siblings,
                      reachability of a null base not yet proven; guard as DiD)

Fix (next): mirror the existing `_base == nullptr` guards at the two confirmed
derefs (and the trivially-parallel `UnderOverAtom`/`MathAtom` siblings), add
regression tests. Keep fuzzing to enumerate any remaining unguarded siblings.

Reproducers saved under `dev/session3-agent3/crashes/`.

