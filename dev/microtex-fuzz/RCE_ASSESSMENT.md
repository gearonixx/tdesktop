# RCE Assessment — MicroTeX Fuzzing (2026-07-02)

**Question:** did the campaign find anything RCE-level?
**Answer:** No. Every finding is denial-of-service (crash / hang). Details below.

| Finding | Primitive observed | Write? | Controlled ptr followed? | Verdict |
|---------|-------------------|--------|--------------------------|---------|
| N1 (ctype-on-`wchar_t`) | OOB **read** → SIGSEGV | no | no | DoS |
| N2 (`insertAtomIntoCol`) | OOB **read** during vector realloc → crash | not demonstrated | no | DoS (heap-corruption risk, unproven) |
| F1 (accent/IndexedArray) | OOB read → garbage `FontInfo*` → crash | no | no (fixed) | DoS, FIXED |
| Hangs (~622/node) | recursion / infinite loop | n/a | n/a | DoS |
| F-OPEN-1 (`\substack`,`\color`) | SEGV only under `-fsanitize=fuzzer` RTTI | n/a | n/a | **Not a bug** (harness artifact) |

## Reasoning

**N1** — glibc `isalpha` indexes `__ctype_b_loc()` by a raw out-of-range `wchar_t`
and reads an unmapped page. There is no write and no attacker-controlled pointer
that later reaches a call/store. It crashes; it cannot hijack control flow.

**N2** — the only finding with any theoretical write angle. What ASan actually
reproduces is a `READ of size 8` during `vector::_M_realloc_insert` relocation. A
write-past-end is reachable in principle, but the insert index is bounded by the
formula's own small `cols()` (not a large attacker integer), the element is a
`shared_ptr`, and **no controlled-write PoC was produced in ~50M executions**.
Classify DoS + heap-corruption risk; do not call it RCE without a write primitive.

**F1** — fixed; was an OOB read returning a garbage pointer that crashed the binary
search. DoS.

**Hangs** — resource exhaustion. Never code execution.

**F-OPEN-1** — the scary-looking one, disproven: identical inputs exit 0 under argv
and AFL non-persistent builds and only SEGV when `-fsanitize=fuzzer` links a second,
divergent copy of C++ RTTI. Zero product impact. See `another_finding.md`.

## Bottom line

Two real, live, fixable memory-safety bugs (N1, N2), both reachable from
attacker-controlled IV markdown math, both **DoS**. No demonstrated path to remote
code execution. Anyone characterizing N1/N2 as RCE is overreaching without a
working write/control primitive.
