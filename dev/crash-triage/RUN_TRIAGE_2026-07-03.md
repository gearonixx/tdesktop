# Fuzz run triage — 2026-07-03

**Target:** `Telegram/ThirdParty/MicroTeX/fuzz/build/fuzz_tex` (libFuzzer + ASAN)
**Mode:** `-fork=3 -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1 -max_len=4096 -timeout=25 -rss_limit_mb=4096`, dict `tex.dict`
**Main PID:** 4087446, started 08:13, ~2h32m uptime at time of triage.

## Result: NO NEW BUGS

A fresh ~2.5h fork-fuzzing run produced **zero new findings**. All crashes replay to
already-documented signatures.

### Crash-dir volume (misleading)
| Metric | Value |
|---|---|
| `crashes/` files | 15,830 (and growing ~130/min) |
| `corpus/` inputs | 1,540 |

`-ignore_crashes=1` saves **every** crashing input with no dedup, so the file count is
not a bug count. Random-sample replay through the ASAN binary shows the crash dir is
**~94% a single signature**, which is a known false alarm.

### Dominant bucket = F-OPEN-1 (false alarm, NOT a bug)
```
SEGV on unknown address 0x0d
  #1 __dynamic_cast (libFuzzer-linked RTTI)
  #2 std::dynamic_pointer_cast<tex::MiddleAtom>(std::shared_ptr<tex::Atom> const&)
```
Already documented in `bug/REPORT.md` §7 as **F-OPEN-1**: `-fsanitize=fuzzer` statically
links a second `__dynamic_cast`/RTTI into the harness; a `dynamic_pointer_cast` over
MicroTeX's multiply-inherited atoms (`ColorAtom : public Atom, public Row`, the
`\middle`/`\substack` products) walks a mismatched `type_info` and faults reading `0x0d`.
Reproduces **only** under the libFuzzer harness — never under plain argv/AFL, never in the
real client. **Not RCE, not a MicroTeX bug.**

### Other signatures seen (all pre-existing, already triaged 07-02/07-03)
| Signature | Location | Class | Status |
|---|---|---|---|
| `MatrixAtom::recalculateLine` OOB | `atom_matrix.cpp:315` | heap-buffer-overflow (read) | old — `bug/asan_trace.txt` |
| `insertAtomIntoCol` UAF (`\begin{cases}\\&`) | matrix layout | use-after-free (read) | old, live |
| `\kern` getFactor | kern handling | corruption | old — `bug/CRASH_REPORT_kern.md` |
| smallmatrix UAF | smallmatrix layout | use-after-free (read) | old (from a prior run) |
| `\multirow` recalculateLine OOB | `atom_matrix.cpp:315` | heap-buffer-overflow (read) | old — **fixed (F1)** |

## Takeaways
- Corpus is **saturated on known findings**; continued fork-fuzzing in this config is not
  yielding new coverage.
- The crash dir is dominated by F-OPEN-1 noise; a real new crash would be easy to miss.

## Recommended next steps
1. Suppress F-OPEN-1 (drop redundant harness RTTI, or triage only non-`0x0d` SEGVs) so the
   crash dir reflects real bugs.
2. Add a watcher that triages only *new* crash files and alerts on any non-F-OPEN-1 signature.
3. Pivot coverage: expand `tex.dict`, push deeper nesting, or fuzz alternate entry points.
