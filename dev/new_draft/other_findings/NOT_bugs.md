# Not bugs / already fixed — disproven or non-actionable

Kept on record so nobody re-files them as findings.

## `dynamic_pointer_cast<MiddleAtom>` SEGV `@0x0d` (a.k.a. F-OPEN-1) — NOT A BUG

**Triggers:** `\substack{a\\b}`, `\color{red}{x}`, `\left(x\middle|y\right)`.
**Claim was:** SEGV reading a vtable at `0x0d` in `Formula::add` (`formula.cpp:110`,
`dynamic_pointer_cast<MiddleAtom>`).
**Reality:** a **libFuzzer harness artifact**, not a MicroTeX defect. Verified this session:

| Build | `\substack{a\\b}` | `\color{red}{x}` | `\left(x\middle\|y\right)` |
|---|---|---|---|
| argv (ASan) | exit 0 | exit 0 | exit 0 |
| AFL non-persistent (ASan) | exit 0 | exit 0 | exit 0 |
| `-fsanitize=fuzzer` | SEGV @0x0d | SEGV @0x0d | SEGV @0x0d |

Only the `-fsanitize=fuzzer` build faults: it links a second, divergent copy of C++ RTTI,
so `dynamic_cast`/`dynamic_pointer_cast` over the multiply-inherited `Atom` hierarchy walks
a foreign `type_info` → SEGV on valid objects. The same AFL build that exits 0 here still
catches real bugs (`\kern` → crash), proving it is the fuzzer runtime, not the input.
**Zero product impact** (Telegram has no libFuzzer runtime). `formula.cpp:110` is indeed
`dynamic_pointer_cast<MiddleAtom>`, but it is not defective.

## `smallmatrix` UAF — ASan tripwire only

**Trigger:** `\begin{smallmatrix}\end{smallmatrix}`. `Environment::operator=` self-assigns
through `env = *(e.copy())` (`atom_matrix.cpp:448`), freeing then reading the same
`Environment` inside one memberwise copy. ASan flags heap-use-after-free; the **release
build exits 0** — the freed bytes are physically intact for the rest of the copy and there
is no realloc window an attacker could steer. Worth a tidy fix (`Environment tmp =
*(e.copy()); env = tmp;`) but **not weaponizable**.

## accent / `IndexedArray` OOB (F1) — already FIXED

`AccentedAtom::createBox` → `FontInfo::getNextLarger` off-by-one OOB read on any accented
Latin-1 char (`$é$`). Fixed in tree; PoC `findings/bug1_accent_oob/poc_min_1byte.bin`
exits 0.

## Inputs that simply do not crash

- `\begin{array}{cc}&\\\\\end{array}` and `\begin{matrix}&\\\\\end{matrix}` — **exit 0**,
  any backslash count 1–6. A previously-circulated "null-deref @ `formula.cpp:25`" label
  was fabricated: line 25 is a logging/registration block in `Formula::_init_()`, not a
  dereference. Do not file.
