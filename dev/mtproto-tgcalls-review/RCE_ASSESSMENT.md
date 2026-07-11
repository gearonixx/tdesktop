# RCE Assessment — MTProto transport + tgcalls glue (2026-07-11)

**Question:** did this review find anything RCE-level?
**Answer:** No. Every finding is DoS / memory-safety-with-no-demonstrated-control-primitive,
and the one item with a real write angle is dead code on our crypto backends. Details below.

| # | Finding | Primitive observed | Write? | Controlled ptr/PC followed? | Verdict |
|---|---------|--------------------|--------|-----------------------------|---------|
| 1 | `parseNotSecureResponse` byte-len-as-count | OOB **read** (attacker-tunable len) → SIGSEGV | no | no | DoS (info-leak potential low), FIXED |
| 2 | FakeTLS Server Hello short-len | `Expects` → `std::terminate` (default); OOB read + ≤27-byte zero-**write** only if contracts compiled out | conditional | no | DoS; heap-corruption risk only in contracts-off builds, FIXED |
| 3 | RSA `decrypt` offset underflow | assert-abort (default); wild `memmove` **write** if contracts off | conditional | no | Latent — **dead code on OpenSSL3/BoringSSL** (res==256) |
| — | `ungzip` uncapped output | allocation growth → OOM | n/a | n/a | DoS (zip-bomb), malicious-server-gated |
| — | `vector_type::read` unbounded count | up-front `QVector` alloc | n/a | n/a | DoS (alloc), previously cleared |

## Reasoning

**#1** — over-read is attacker-tunable in size (length field × 4) but lands in `resPQ.pq` /
key fingerprints, which are consumed by local nonce/fingerprint checks and never echoed to
the network. No write, no attacker-controlled pointer reaches a store or call. It crashes;
it cannot hijack control flow. Fixed. ASan repro: `findings/notsecure_oob_read/asan_report.txt`.

**#2** — the interesting one for a write angle. What actually happens first, on a default
tdesktop build, is `Expects(5 >= 32)` → `std::terminate` (GSL contracts are on; no
`GSL_*_ON_CONTRACT_VIOLATION` is defined project-wide), i.e. a clean unauthenticated remote
**crash before** the digest zero-write executes. The OOB zero-write (≤27 bytes past
`_incoming`) only materializes in a build that compiles GSL contracts out, and even then it
writes **zeros** at a fixed forward offset — not attacker-controlled bytes and not a pointer
— so there is no path to controlled corruption of a live object, let alone PC control. DoS;
heap-corruption risk is contracts-off-only and low. Fixed.

**#3** — has a genuine wild `memmove` **write** primitive when driven (PoC:
`findings/rsa_decrypt_offset/`, ASan shows a 255-byte heap OOB WRITE with contracts off). But
(a) it is **unreachable on OpenSSL 1.1+/3.x and BoringSSL**, which return `res == 256` so the
branch is never entered; and (b) even if reached, the moved bytes are the RSA-recovered
plaintext at a fixed negative offset, not attacker-chosen placement of attacker-chosen
bytes at an attacker-chosen target. On a default build it is an assert-abort. Not RCE; fix
it as defense-in-depth.

**DoS residuals** — `ungzip` (no output cap) and `vector_type::read` (unbounded up-front
alloc) are resource-exhaustion, malicious-server-gated, never code execution. The v2
signaling path already caps its gunzip at 2 MiB; mirror that.

## Bottom line

Two real, live, fixable memory-safety bugs (#1, #2), both pre-auth and both **DoS** on a
default build (with a contracts-off-only, non-controlled write angle on #2). One latent
write bug (#3) that is dead code on the crypto libraries we ship. No demonstrated path to
remote code execution in either focus area. Anyone characterizing these as RCE is
overreaching without a working write/control primitive — none was produced.
