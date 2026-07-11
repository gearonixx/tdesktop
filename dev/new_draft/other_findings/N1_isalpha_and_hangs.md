# Finding #6 (N1) — raw `wchar_t` into `<cctype>` · and the hang bucket

## N1 — `isalpha(wchar_t)` out-of-range read (DoS / benign-in-release)

**Trigger:** an astral (non-BMP) codepoint inside a command/env name, e.g. after
`\begin{env}…` (99-byte PoC `findings/new/pocA_isalpha_segv.tex`).
**Site:** `isalpha` ← `TeXParser::isValidCharInCmd` `src/core/parser.h:354`
(also `isValidName`).

### Root cause
glibc `isalpha`/`isalnum` index `__ctype_b_loc()[c]`, defined only for `c ∈ [-1, 255]`.
MicroTeX passes a raw `wchar_t` (up to U+10FFFF) straight in, so a large codepoint reads
far outside the ctype table.

### gdb behavior — value-dependent, and **benign in the release build**
- Under **ASan**: wild OOB read → SIGSEGV (this is how it was found).
- Under the **release** build: the PoC **exits 0** — the out-of-range index happens to
  land on mapped memory, the garbage classification result merely mis-parses, no crash.

So N1 is a latent OOB read that is a reliable crash only under sanitizers; in the shipping
build it is closer to undefined-but-non-faulting behavior. Lowest priority. **Fix:** mask
to `unsigned char` / guard `c < 128` before calling `<cctype>`, or use a Unicode-aware
predicate.

## Hang bucket — resource-exhaustion DoS

The largest crash-corpus bucket (~1,438 `TIMEOUT_or_SILENT` files) is deep parser
recursion / pathological loops — CPU/stack exhaustion, not memory safety. Un-minimized.
`rip`/fault n/a (timeout, not signal). **DoS.** Mitigation: input size / nesting-depth caps
in the parser (some caps already exist: `kMaxArrayCells`, `kMaxArrayRowSpan`).

Neither of these transfers control or corrupts memory; both are strictly denial-of-service.
