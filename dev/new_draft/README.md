# MicroTeX / Telegram Desktop — Security Findings Draft

**Status:** DRAFT (internal). Not yet reported upstream.
**Date:** 2026-07-03
**Target:** `tex::LaTeX::parse` in bundled MicroTeX, as reached by Telegram Desktop's
Instant View (IV) markdown math renderer
(`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`).
**Attack surface:** attacker-controlled LaTeX in `$…$` / `$$…$$` math spans of an IV
article — rendered on view, **near-zero-click**.

---

## What this draft is

A consolidated, honest write-up of every crash found across three AFL++ campaigns
(~100M+ executions) plus binary triage of 100 real Telegram Desktop crash dumps, all
re-verified in this session with:

- a **release (non-ASan) build** driven under **gdb** to get the *true* faulting
  instruction and `rip`, and
- **live reproduction in the real Telegram Desktop client**, correlated against the
  on-disk crash dumps.

The headline is a correction of an earlier "all DoS, nothing dangerous" verdict.

## Headline

| | |
|---|---|
| **1 control-flow hijack primitive** | `\kern` → `SpaceAtom::getFactor` indirect call through out-of-bounds memory. **Confirmed in the shipping client** (execution transfers to a per-run-varying **heap** address). Strongest RCE candidate; RCE **not** demonstrated. **UNFIXED / live in 6.9.x.** |
| **~6 DoS bugs** | multirow null-deref, N2 `insertAtomIntoCol` OOB read, N4 ragged `createBox` (hardened → abort), a null-deref cluster (~4 confirmed sites), N1 `wchar_t`→ctype, plus a large hang bucket. All crash-only. |
| **2 non-bugs** | `\substack`/`dynamic_pointer_cast @0x0d` (libFuzzer RTTI artifact) and the accent/IndexedArray bug (already fixed). |
| **RCE demonstrated?** | **No.** One genuine control-flow-hijack *primitive* (`\kern`); no working write/target-control PoC produced. |

## Navigation

- [`00_EXECUTIVE_SUMMARY.md`](00_EXECUTIVE_SUMMARY.md) — the one-page version.
- [`01_METHODOLOGY.md`](01_METHODOLOGY.md) — builds, gdb procedure, dump-triage tooling, honesty controls.
- [`02_ALL_FINDINGS.md`](02_ALL_FINDINGS.md) — master table: every finding, gdb signature, verdict.
- [`03_CRASH_DUMP_CORRELATION.md`](03_CRASH_DUMP_CORRELATION.md) — 100 real dumps; the 4 `\kern` control-flow dumps; live-repro confirmation.
- [`kern/`](kern/) — **the `\kern` control-flow hijack, in full depth** (start at [`kern/README.md`](kern/README.md)).
- [`other_findings/`](other_findings/) — one file per DoS finding and per non-bug.

## One-line honest verdict

> A remotely-reachable **control-flow hijack primitive** (`\kern`) exists and is
> confirmed live in shipping Telegram Desktop; it currently crashes rather than
> executes attacker code, so it is a **credible RCE candidate, not a demonstrated
> RCE**. Everything else is denial-of-service.
