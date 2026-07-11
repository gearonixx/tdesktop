# MTPROTO-3 — Pre-auth remote DoS: non-16-aligned `encrypted_answer` aborts in AES-IGE

**Severity:** Medium–High (remote, **pre-authentication**, deterministic crash / DoS).
**Class:** unchecked length / unhandled library precondition → `abort()` (CWE-617, CWE-248).
**Status:** **NEW this pass.** Fix applied to the working tree (uncommitted) — the gate
at `:605` is widened `& 0x03` → `& 0x0F`. Present in upstream Telegram Desktop as well as
this fork (an upstream robustness gap we also ship, not a fork regression).
**File:** `Telegram/SourceFiles/mtproto/details/mtproto_dc_key_creator.cpp:604-634`
(`DcKeyCreator::dhParamsAnswered`, the `server_DH_params_ok` handler).

---

## Summary

During auth-key creation (MTProto 2.0 DH handshake), the client decrypts the
server-supplied `encrypted_answer` of `server_DH_params_ok` with AES-256-IGE. The
length of that string is validated **only** as a multiple of 4 bytes and `≥ 24`
bytes — but AES-IGE (OpenSSL `AES_ige_encrypt`) requires the length to be a multiple
of the **16-byte** block size and calls `OPENSSL_assert((length % 16) == 0)`, which
resolves to `OPENSSL_die()` → `abort()`. A malicious server / MITM / MTProto proxy
sends an `encrypted_answer` of length 24 (or 28, 40, 44, …) and the client process
aborts **before any authorization key exists**.

## Root cause — a 4-byte gate in front of a 16-byte requirement

```cpp
// mtproto_dc_key_creator.cpp:603-609
auto &encDHStr = data.vencrypted_answer().v;            // attacker-controlled TL string
uint32 encDHLen = encDHStr.length(), encDHBufLen = encDHLen >> 2;
if ((encDHLen & 0x03) || encDHBufLen < 6) {             // %4 and >=6 primes ONLY
    ...
    return failed();
}
...
// mtproto_dc_key_creator.cpp:634
aesIgeDecryptRaw(encDHStr.constData(), &decBuffer[0], encDHLen, aesKey.data(), aesIV.data());
```

`aesIgeDecryptRaw` (`mtproto_auth_key.cpp:161-169`) calls OpenSSL directly:

```cpp
AES_ige_encrypt(src, dst, len, &aes, aes_iv, AES_DECRYPT);   // len == encDHLen
```

OpenSSL `crypto/aes/aes_ige.c:60`:

```c
OPENSSL_assert((length % AES_BLOCK_SIZE) == 0);   // AES_BLOCK_SIZE == 16
```

`OPENSSL_assert` is **not** gated on `NDEBUG` — it is OpenSSL's own always-on check
that calls `OPENSSL_die()` → `abort()`. So `encDHLen ∈ {24, 28, 40, 44, 56, 60, …}`
(i.e. `≡ 8 or 12 (mod 16)`, `≥ 24`) — all of which pass the `& 0x03` gate — abort the
process. Roughly half of all 4-aligned lengths `≥ 24` trigger it.

The sibling encrypted-message path is **not** affected: `SessionPrivate::handleReceived`
masks the payload to 16-byte alignment before decrypting
(`session_private.cpp:1303`, `encryptedIntsCount = (intsCount - header) & ~0x03U`,
i.e. a whole number of 4-prime = 16-byte blocks). Only the DH handshake trusts a raw
`% 4` length.

## Reachability (pre-auth, malicious server / MITM / proxy)

`dhParamsAnswered` runs only in `Stage::WaitingDH`, so the attacker drives the
plaintext handshake two round-trips deep — all before an auth key exists:

1. Client sends `req_pq_multi`. Attacker replies with a `resPQ` carrying (a) a `pq`
   that factorizes (any small semiprime) and (b) a `server_public_key_fingerprints`
   entry equal to one of Telegram's well-known embedded RSA fingerprints. `pqAnswered`
   succeeds and the client sends `req_DH_params`, entering `Stage::WaitingDH`.
2. Attacker replies with `server_DH_params_ok` echoing the client's `nonce` and its own
   `server_nonce` (both seen in cleartext), and an `encrypted_answer` of length **24**.
   The `& 0x03` gate passes → `AES_ige_encrypt(len=24)` → **`abort()`**.

Both `nonce` and `server_nonce` are transmitted in the clear during the plaintext
handshake, so a MITM or a malicious/compromised MTProto proxy
(`core.telegram.org/proxy`), not just the DC operator, can supply them. This is the
same threat model and severity class as the already-fixed MTPROTO-2 (a deterministic
pre-auth abort).

## Impact

Deterministic remote termination of the client during DC key creation → **DoS**. It is
a clean `abort()` (SIGABRT) with a stack guard, not memory corruption: `decBuffer` is
sized to exactly `encDHLen` bytes, so had OpenSSL not asserted, the decrypt would have
stayed in-bounds. No write primitive, no info leak. Severity is driven by *pre-auth
remote crash*, tunable to fire on essentially every connection attempt to a hostile
endpoint (login, reconnect, new DC).

## Proof of concept (demonstrated)

`ige_test.c` — 15-line proof that OpenSSL aborts on `len % 16 != 0`.
`poc_dh_ige_abort.cpp` — faithfully models the `dhParamsAnswered` gate then performs
the exact `aesIgeDecryptRaw` call on a 24-byte `encrypted_answer`.

```
$ bash repro.sh
--- len=24 (4-aligned, NOT 16-aligned; passes the client gate) ---
crypto/aes/aes_ige.c:60: OpenSSL internal error: assertion failed: (length % AES_BLOCK_SIZE) == 0
Aborted (core dumped)          exit=134
...
--- BUGGY gate (current: encDHLen & 0x03) ---
  gate ACCEPTED encrypted_answer (len=24, len%16=8); calling AES-IGE...
Aborted (core dumped)          exit=134   (remote pre-auth DoS)
--- FIXED gate (proposed: encDHLen & 0x0F) ---
  gate rejected encrypted_answer (len=24) -> handshake fails, no crash   exit=0
```

Verified against the system OpenSSL the build links (OpenSSL 3.6.2; tdesktop pins its
own OpenSSL 3.x — same `AES_ige_encrypt` assertion, unchanged since OpenSSL 0.9.8).

## Fix (minimal, one token)

Widen the alignment gate from 4 to 16 bytes so the length matches AES-IGE's precondition:

```cpp
// mtproto_dc_key_creator.cpp:605
if ((encDHLen & 0x0F) || encDHBufLen < 6) {   // was `& 0x03`
```

`0x0F` implies `0x03`, and legitimate `encrypted_answer`s are always a 16-byte multiple
(the server pads `answer_with_hash` to a multiple of 16), so no valid handshake is
affected. `encDHBufLen < 6` still guarantees the `&decBuffer[5]` / `end = from +
(encDHBufLen-5)` reads below stay in-bounds. (Defense-in-depth alternative: add a
`length % 16 == 0` guard inside `aesIgeDecryptRaw`/`aesIgeEncryptRaw` so no future
caller can reintroduce this — but the one-token gate fix is the minimal change.)

## Regression test

Drive `DcKeyCreator` (or `dhParamsAnswered` directly, with the attempt forced to
`Stage::WaitingDH` and matching nonces) with a `server_DH_params_ok` whose
`encrypted_answer` is 24 bytes; assert `failed()` fires and `AES_ige_encrypt` is never
called (no abort). Pre-fix the test aborts; post-fix it returns cleanly.
