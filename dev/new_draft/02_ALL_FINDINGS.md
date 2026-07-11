# Master Findings Table

All triggers verified this session against the **release** build under gdb, unless noted.
"Shape" is the gdb-observed nature of the fault, which is what separates DoS from a
control-flow issue.

## Live / real bugs

| # | Name | Minimal trigger | Crash site | gdb shape (release) | `rip` | Class | Fixed? |
|---|------|-----------------|-----------|---------------------|-------|-------|--------|
| **1** | **`\kern` / getFactor** | `\kern` | `SpaceAtom::getFactor` `atom_space.h:45` | **indirect call through OOB `std::function`; control transfers to a per-run-varying HEAP address** | **`rip == fault`, in heap (wild)** | **Control-flow hijack primitive** | ❌ live in 6.9.x |
| 2 | multirow | `\begin{array}{c}\multirow{9}{*}{x}\end{array}` | `MatrixAtom::recalculateLine` `atom_matrix.cpp:315` | null-deref `shared_ptr::get(this=0)`; OOB *write* two lines later is **gated behind this deref and never reached** | valid code, fault `0x0` | DoS | ✅ local tree only (`min(_i+_n,rows)`) |
| 3 | N2 `insertAtomIntoCol` | `\begin{cases}\\&\end{cases}` | `ArrayFormula::insertAtomIntoCol` `formula.cpp:214` | OOB read at a **page boundary** during `vector::_M_realloc_insert` relocation | valid code, fault `0x…000` | DoS (heap-corruption risk, unproven) | ❌ |
| 4 | N4 ragged `createBox` | `\begin{tabular}0\\&\hline` | `MatrixAtom::createBox` `atom_matrix.cpp:595` | **`_GLIBCXX_ASSERTIONS`** `std::vector::operator[]` `__n < size()` → clean `abort()` | libc (abort) | DoS (hardened) | ❌ (mitigated by assertions) |
| 5 | null-deref cluster | `\c{}{2}` · `\begin{array}{}\multicolumn{2}{` · UnderOver · `frac{\tiny ` | Cedilla `atom_impl.h:76`, Multicolumn `atom_matrix.cpp:691`, UnderOver `atom_basic.h:528`, ScaleAtom `atom_basic.h:115` | null-deref | valid code, fault `0x0`/`0x8` | DoS | ❌ (likely one root cause) |
| 6 | N1 `wchar_t`→ctype | astral codepoint after `\begin{env}` | `isalpha` ← `isValidCharInCmd` `parser.h:354` | ASan: wild OOB read → SEGV. **Release: exits 0** (index hits mapped ctype memory) | — | DoS / benign-in-release | ❌ |
| 7 | hangs | recursion / pathological loops | parser | timeout (~1,438 corpus files) | — | DoS (resource exhaustion) | ❌ |

## Not bugs / already fixed

| Name | Trigger | Why it's not actionable |
|---|---|---|
| `dynamic_pointer_cast<MiddleAtom>` SEGV `@0x0d` (a.k.a. F-OPEN-1) | `\substack{a\\b}`, `\color{red}{x}`, `\left(x\middle\|y\right)` | **libFuzzer RTTI artifact.** Exits 0 under argv + AFL (same ASan allocator); only faults when `-fsanitize=fuzzer` links a second copy of C++ RTTI that diverges on the multiply-inherited Atom hierarchy. No product impact. |
| accent / `IndexedArray` OOB (F1) | any accented Latin-1, `$é$` | Already **fixed**; PoC `findings/bug1_accent_oob/poc_min_1byte.bin` exits 0. |
| smallmatrix UAF | `\begin{smallmatrix}\end{smallmatrix}` | ASan-only tripwire (`Environment::operator=` self-assign). **Exits 0 in release** — freed bytes still intact for the remainder of the copy; no realloc window. |
| two-line multirow | `\begin{array}{c}\multirow{5}{*}{x}\end{array}\n\begin{matrix}\multirow{20}{*}{x}\end{matrix}` | **Identical** crash to finding #2 (same `rip`, same `atom_matrix.cpp:315`, same fault `0x0`). The second formula never renders — the first array's multirow crashes first. Not a distinct bug. |
| `\begin{array}{cc}&\\\\\end{array}` / `\begin{matrix}&\\\\\end{matrix}` | as written | **Do not crash** (exit 0, any backslash count 1–6). A previously-circulated "null-deref @ formula.cpp:25" label was fabricated — that line is a logging block. |

## Severity ordering (by gdb-observed shape, worst first)

1. **`\kern`** — control transferred to a wild heap address (`rip == fault`). *Only*
   finding that violates control-flow integrity. → **HIGH**, candidate Critical.
2. **N2** — OOB read during vector reallocation (raw internal path, not assertion-guarded).
3. **multirow** — null-deref on raw `sptr<Box>**`/`float[]` arrays; write unreachable.
4. **null-deref cluster** — null-deref in app code, fixed low fault address.
5. **N4** — same *family* as multirow but on a `std::vector`, so `_GLIBCXX_ASSERTIONS`
   converts it to a clean abort (safest).
6. **N1 / hangs** — value-dependent read / resource exhaustion.
