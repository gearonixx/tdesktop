# Executive Summary

**Product:** Telegram Desktop (Linux, x86-64), tested against build in
`/home/x/c/tdesktop`, module range `0x555555554000–0x55555d561000`.
**Component:** bundled MicroTeX (`Telegram/ThirdParty/MicroTeX`).
**Entry point:** `tex::LaTeX::parse`, reached from Instant View markdown math
(`iv_markdown_microtex.cpp:280`). Attacker-controlled `$…$` math in an IV article is
parsed and rendered when the victim opens the article — **near-zero-click**.

---

## The one finding that matters: `\kern` → control-flow hijack

A five-byte payload — **`\kern`** (with no length argument) — deterministically
redirects the instruction pointer of the shipping Telegram Desktop process to an
address the code reads out of **out-of-bounds memory**.

- **Root cause.** `\kern` with no length yields `UnitType::none`, whose integer value
  is **−1** (`enums.h:123`). `SpaceAtom::getFactor` then evaluates
  `_unitConversions[static_cast<i8>(none)] (env)` = `_unitConversions[-1](env)`
  (`atom_space.h:45`) — it reads the array element **one slot before** the array,
  reinterprets those bytes as a `std::function`, and **calls it**.
- **Observed in the real client.** Under gdb and via four real crash dumps, the call
  transfers control to a **heap address that varies every run**
  (`0x55556ee671a0`, `0x555570455100`, `0x555573213180`, `0x555574ca9e00`) — i.e. the
  target is pulled from live memory, not fixed code. The instruction pointer *becomes*
  the fault address (`rip == fault`), which is the textbook control-flow-hijack crash
  signature.
- **Uniqueness.** Of **100** crash dumps on disk (spanning 2026-03-14 → 2026-07-03),
  **exactly the four `\kern` dumps** show this signature. Every other dump — including
  the 89 in the prior triage — crashes with `rip` in valid code (safe DoS).
- **Determinism.** Reproduced on demand by pasting `\kern`; this **rules out** any
  hardware/thermal bit-flip explanation.

**What it is:** a remotely-reachable, deterministic **control-flow hijack primitive**
in shipping Telegram Desktop.
**What it is not (yet):** demonstrated remote code execution. The transfer currently
lands on heap bytes that decode to a faulting instruction → the process crashes. Turning
this into code execution requires showing the jump target (or the `std::function`
`_M_invoker` slot it is read from) can be made to hold **attacker-controlled** bytes —
not demonstrated here.

**Severity recommendation:** **HIGH** (control-flow integrity violation, remote,
near-zero-click) pending exploit-development review that could raise it to Critical.
Full dossier: [`kern/`](kern/).

---

## Everything else is denial-of-service

| Finding | Trigger | gdb signature (release build) | Class |
|---|---|---|---|
| multirow | `\begin{array}{c}\multirow{9}{*}{x}\end{array}` | `rip` valid; null-deref `shared_ptr::get(this=0)` @ `atom_matrix.cpp:315` | DoS |
| N2 `insertAtomIntoCol` | `\begin{cases}\\&\end{cases}` | `rip` valid; OOB read at page boundary during `vector::_M_realloc_insert`, `formula.cpp:214` | DoS |
| N4 `createBox` | `\begin{tabular}0\\&\hline` | `SIGABRT`, `_GLIBCXX_ASSERTIONS` `__n < size()` @ `atom_matrix.cpp:595` (hardened → safe) | DoS |
| null-deref cluster | `\c{}{2}`, `\begin{array}{}\multicolumn{2}{`, … | `rip` valid; null-deref @ `0x0`/`0x8` in Cedilla/Multicolumn/UnderOver/ScaleAtom | DoS |
| N1 `wchar_t`→ctype | astral codepoint in `\begin{env}` | ASan SEGV; **exits normally in release** (read hits mapped memory) | DoS / benign-in-release |
| hangs | deep recursion / loops | timeout; ~1,438 files | DoS |

## Two things that are NOT bugs

- **`\substack{a\\b}` / `dynamic_pointer_cast<MiddleAtom>` SEGV @`0x0d`** — a
  **libFuzzer RTTI artifact**. Exits 0 under argv and AFL builds; only faults when
  `-fsanitize=fuzzer` links a second, divergent copy of C++ RTTI. Zero product impact.
- **accent / `IndexedArray` OOB** — already **fixed** in tree; PoC exits 0.

## Corrected record

An earlier pass characterized the whole campaign as "all DoS, nothing dangerous." That
was **wrong for `\kern`**. The error came from trusting an ASan/harness repro where the
`_unitConversions[-1]` slot happened to resolve to `0x0` (a clean crash), instead of
verifying against the real client — where the same slot resolves to a heap pointer and
the process **transfers control into the heap**. The correction was forced by driving
the release build under gdb and by the user's live reproduction.
