# Finding #5 — null-pointer-dereference cluster (DoS)

A family of unconditional atom dereferences: many `createBox()` / accessor implementations
assume a child/base atom pointer is non-null, but the parser can produce a null
`sptr<Atom>` from malformed/empty macro arguments, which propagates unchecked. **CWE-476,
DoS.** `rip` stays in valid code; fault at a fixed low address (`0x0`/`0x8`). Likely **one
upstream root cause** (empty macro argument → null Atom) surfacing at many sinks.

## Confirmed sites (gdb, release build)

| Site | Function | Minimal trigger | Fault |
|---|---|---|---|
| `atom_impl.h:76` | `CedillaAtom::createBox` | `\c{}{2}` | null-deref `_base->createBox`, `si_addr=0x0` |
| `atom_matrix.cpp:691` | `MulticolumnAtom::createBox` | `\begin{array}{}\multicolumn{2}{` | null-deref, `si_addr=0x0` (highest-count site) |
| `atom_basic.h:528` | `UnderOverAtom::leftType` (via `Dummy::leftType`) | `poc_underover_*.bin` | null-deref, `si_addr=0x0` |
| `atom_basic.h:115` | `ScaleAtom::ScaleAtom` ctor (via `MonoScaleAtom`, `\tiny`-class) | `frac{\tiny ` | null-deref, `si_addr=0x8` |

Example gdb frame (Multicolumn):
```
#0 MulticolumnAtom::createBox  atom_matrix.cpp:691   rip valid; si_addr=0x0
#1 MatrixAtom::createBox       atom_matrix.cpp
```
All four: `rip` in valid application code, small fixed fault address → safe DoS crashes,
same tier as the other null-derefs. No attacker data at the fault address, no write, no
control transfer.

## Suggested fix

Prefer a single choke-point: where macro arguments are parsed into `Atom` pointers,
substitute `EmptyAtom` for a null result (MicroTeX already does this in the partial-parse
path, `formula.cpp:50`). That would likely close most of the cluster with one change.
Failing that, null-check each sink (`if (_base == nullptr) return EmptyAtom…`). ~15 further
sites were bucketed but not individually root-caused; confirm how many are truly distinct
before filing counts.
