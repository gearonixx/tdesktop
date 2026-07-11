# `\kern` — Control-Flow Hijack in MicroTeX `SpaceAtom::getFactor`

The most serious finding of the investigation. A 5-byte LaTeX payload deterministically
redirects the instruction pointer of shipping Telegram Desktop to an out-of-bounds
value, transferring control into the heap.

## Read in this order

1. **[`00_DOSSIER.md`](00_DOSSIER.md)** — the complete, self-contained report to send to
   developers. Root cause, source lines, GDB layout, register analysis, call chain,
   real-client dumps, exploitability, fix, PoC. **If you read one file, read this.**
2. [`01_MECHANISM.md`](01_MECHANISM.md) — deep mechanics of the `std::function[-1]`
   invocation and the libstdc++ call ABI, for reviewers who want the byte-level detail.
3. [`02_DUMP_EVIDENCE.md`](02_DUMP_EVIDENCE.md) — the four real crash dumps in full:
   registers, stackwalk, `rip` variation, faulting instruction.
4. [`03_EXPLOITABILITY.md`](03_EXPLOITABILITY.md) — honest control-flow-hijack-vs-RCE
   analysis and what would be required to weaponize it.
5. [`04_PROPOSED_FIX.md`](04_PROPOSED_FIX.md) — the bounds-check patch (proposed; **not
   applied**).
6. [`poc/poc_kern.tex`](poc/poc_kern.tex) — the 5-byte proof of concept (`\kern`).

## One paragraph

`\kern` with no length argument produces a `SpaceAtom` whose unit is `UnitType::none`.
`none` has the integer value **−1**. At render time `SpaceAtom::getFactor` evaluates
`_unitConversions[static_cast<i8>(none)](env)` = `_unitConversions[-1](env)`: it reads the
element **one slot before** a global array of `std::function`, reinterprets those bytes as
a `std::function<float(const Environment&)>`, and **calls it**. In the shipping client the
invoker pointer read from that slot points into the **heap** and **varies every run**;
control transfers there and the process crashes trying to execute heap data. Confirmed by
gdb on the release build and by four real Telegram Desktop crash dumps, two of them
reproduced live on demand. **Control-flow hijack primitive; RCE not demonstrated.**
