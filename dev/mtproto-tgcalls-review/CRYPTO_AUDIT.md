# MTProto cryptographic-boundary audit (2026-07-11)

Question posed: *is there a math/crypto weakness in the MTProto implementation?* This is a
boundary-by-boundary review of the handshake + record-layer crypto against the published
MTProto 2.0 spec and the **Security Guidelines for Client Software Developers**. Verdict up
front: **the cryptographic core is a faithful, correct implementation — no crypto break.**
The only real defects are in the *framing/glue* (see `SESSION_FINDINGS.md`: M-1 OOB read,
M-2 FakeTLS DoS, latent RSA-decrypt offset), not in the math. That is the expected and
honest outcome for a mature client; the value of this pass is *confirming* each check is
present **and wired**, since a Security-Guideline check that exists but isn't called is
worthless.

## Boundary-by-boundary verdict

| # | Boundary | Spec requirement | Code | Verdict |
|---|----------|------------------|------|---------|
| 1 | DH prime `p` | 2048-bit, prime, safe (`(p-1)/2` prime) | `mtproto_dh_utils.cpp:17` `IsPrimeAndGoodCheck` | ✓ correct |
| 2 | Generator `g` | quadratic-residue congruence per `g∈{2..7}` | `mtproto_dh_utils.cpp:35-76` | ✓ correct |
| 3 | Prime check **wired** | must run on server's `dh_prime` before use | `mtproto_dc_key_creator.cpp:668` `IsPrimeAndGood(...)` | ✓ called pre-use |
| 4 | `g_a` (server) range | `2^{2048-64} ≤ g_a ≤ p−2^{2048-64}` | `IsGoodModExpFirst` via `CreateAuthKey` `:713` | ✓ correct |
| 5 | `g_b` (client) range | same bounds, else regenerate | `CreateModExp` loop `dh_utils.cpp:144-157` | ✓ correct |
| 6 | RSA PQ padding | `RSA_PAD` IND-CCA construction | `EncryptPQInnerRSA` `dc_key_creator.cpp:232` | ✓ correct |
| 7 | RSA input `< N` | retry until `key_aes_encrypted < modulus` | `IsGoodEncryptedInner` `:209` (256-byte BE compare) | ✓ correct |
| 8 | RSA keys pinned | server keys are client-side constants | `mtproto_dc_options.cpp:61` built-in PEM | ✓ pinned |
| 9 | Record KDF | MTProto 2.0 SHA256 KDF, `x=send?0:8` | `AuthKey::prepareAES` `auth_key.cpp:78` | ✓ exact match |
| 10 | `msg_key` on decrypt | recompute `SHA256(key₈₈…‖plaintext)` mid-128, compare | `session_private.cpp:1322-1334` | ✓ **constant-time** |
| 11 | Length/padding | `msg_len%4==0`, `12≤pad≤1024`, in-buffer | `session_private.cpp:1336-1342` | ✓ correct |
| 12 | `auth_key_id` | low 64 bits of SHA1(auth_key) | `AuthKey::countKeyId` `:144` | ✓ correct |
| 13 | Nonce / `new_nonce_hash` | match nonces; verify `new_nonce_hash{,1,2}` | `dc_key_creator.cpp:643-651,688,763,785` | ✓ correct |
| 14 | Replay / session | `session==_sessionId`, msg_id monotonic window | `session_private.cpp:1352`, `_receivedMessageIds` | ✓ correct |
| 15 | AES primitives | AES-256-IGE (record), AES-256-CTR (obfusc.) | `aesIge*Raw`/`aesCtrEncrypt` `auth_key.cpp:151` | ✓ OpenSSL |

## The three checks that actually matter (why the protocol is safe)

**A. `msg_key` recomputation (boundary #10) — the AEAD guarantee.** MTProto 2.0 is
encrypt-then-MAC-ish: `msg_key` is the middle 128 bits of `SHA256(auth_key[88+x : 120+x] ‖
plaintext_with_padding)`. On receipt the client decrypts, **recomputes** that SHA256 over
the *decrypted* bytes, and compares to the `msg_key` that was sent in the clear —
`ConstTimeIsDifferent(&msgKey, sha256+8, 16)` at `session_private.cpp:1331`. Because the
AES key/iv are *derived from* `msg_key`, and `msg_key` is bound to both the plaintext and a
secret slice of `auth_key`, an attacker cannot forge or tamper a ciphertext without knowing
`auth_key`. The comparison is **constant-time**, so there is no timing oracle on the tag.
This is implemented correctly; it is the single most important line in the record layer.

**B. DH parameter validation (boundaries #1–5) — no small-subgroup / weak-key.** A
malicious DC could otherwise send a composite or non-safe `p`, or a boundary `g_a` (0, 1,
`p−1`) to force a tiny shared-secret space. The client rejects all of these: `p` must be a
2048-bit safe prime with the right `g`-congruence (`IsPrimeAndGood`, actually *called* at
`:668`), and both `g_a` and `g_b` must sit in `[2^{1984}, p−2^{1984}]` (`IsGoodModExpFirst`).
These are exactly the Security-Guideline checks.

**C. RSA_PAD + `< N` retry + pinned keys (boundaries #6–8) — no RSA malleability / no
MITM.** The PQ inner data is wrapped in the post-2021 `RSA_PAD` scheme (reversed padded
data ‖ `SHA256(temp_key‖data)`, AES-256-IGE under a random `temp_key`, `temp_key ⊕
SHA256(aes_encrypted)`), retried until the 256-byte value is `< modulus`, then `x^e mod N`
to Telegram's **pinned** public key. Pinning is what makes the whole handshake a real
authentication: only the holder of the DC private key can complete it.

## What this means for the threat model

There is no realistic "hard-math" break to find here. The productive attack surface is
**not** the crypto — it is the *plaintext-side byte handling* around it:
- the pre-auth **framing** parsers (`parseNotSecureResponse`, the FakeTLS hello) — where
  M-1/M-2 live, both fixed;
- the RSA glue offset math (latent #3);
- post-decrypt TL interpretation and the accepted **alloc/zip-bomb DoS** residual.

Reaching even those requires a malicious/MITM server or proxy (there is no anonymous-remote
path), consistent with the rest of the review.

## Verified references
- `mtproto_dh_utils.cpp:17-103` — `IsPrimeAndGoodCheck` / `IsGoodModExpFirst` (prime, safe-prime, g-congruence, `2^{2048-64}` bounds).
- `mtproto_dc_key_creator.cpp:209-324` — `IsGoodEncryptedInner` / `EncryptPQInnerRSA` (RSA_PAD + `<N`).
- `mtproto_dc_key_creator.cpp:668` — `IsPrimeAndGood` call site (wiring).
- `mtproto_dc_key_creator.cpp:713` — `CreateAuthKey` → `IsGoodModExpFirst(g_a)`.
- `mtproto_auth_key.cpp:78-104` — `prepareAES` (2.0 KDF) + `partForMsgKey` (x offset).
- `mtproto_auth_key.cpp:144-149` — `countKeyId` (SHA1 low-64).
- `session_private.cpp:1322-1342` — `msg_key` recompute + const-time compare + length bounds.
- `mtproto_dc_options.cpp:61` — pinned RSA public keys (built-in PEM).
