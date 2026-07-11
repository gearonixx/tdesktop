# MTProto transport + tgcalls / lib_webrtc — Session Findings (defensive robustness review)

Date: 2026-07-11. Branch: `fix/mtproto-preauth-oob` (HEAD `4467e0cbf1`).
Target: Telegram Desktop fork — the **network-facing byte handlers** that must be
memory-safe on malformed / adversarial input before release:
**MTProto transport / TL deserialization** (`Telegram/SourceFiles/mtproto/`) and
**tgcalls / lib_webrtc** call transport (`Telegram/ThirdParty/tgcalls/`,
`Telegram/lib_webrtc/`).

Method this pass: **static tracing of the read/deserialize paths** (bytes → framing
gate → decrypt → TL/parse handler), plus **targeted empirical PoCs** where a library
precondition was in doubt. Reachability confirmed before assigning impact. This is
"iteration #4" of an ongoing thread; it re-verifies the committed fixes, re-confirms the
still-open items, pushes into the seams prior passes flagged *unexamined*, and adds one
**new, demonstrated** finding.

Honest threat model (unchanged): both areas need a malicious/MITM server, a malicious
MTProto proxy, or a negotiated call to reach. The realistic ceiling here is **DoS /
logic**, not remote memory corruption or RCE. Findings are scored accordingly.

Companion docs (this folder): `FINDING-mtproto-3-ige-align.md` (new finding, full
writeup + PoC), `poc_dh_ige_abort.cpp`, `ige_test.c`, `repro.sh`.
Prior long-form reports: `../REVIEW-robustness-2026-07-11.md`,
`../REVIEW-mtproto-tgcalls-2026-07-11.md`,
`../mtproto-tgcalls-audit/SESSION_FINDINGS.md`,
`../mtproto-tgcalls-review/SESSION_FINDINGS.md`.

---

## TL;DR — iteration #4 (this pass) — **MTPROTO-3: new pre-auth remote DoS**

**A third pre-auth memory/robustness defect in the MTProto plaintext handshake, of the
same class and severity as the already-fixed MTPROTO-2: a malicious server / MITM /
MTProto proxy crashes the client during auth-key creation, before any key exists.**

- **Where:** `DcKeyCreator::dhParamsAnswered` — the `server_DH_params_ok` handler
  (`mtproto/details/mtproto_dc_key_creator.cpp:604-634`).
- **Bug (units/precondition mismatch):** the server-controlled `encrypted_answer` length
  is validated only as `encDHLen % 4 == 0 && encDHLen/4 >= 6` (`:605`), then fed as-is to
  AES-256-**IGE** (`aesIgeDecryptRaw` → OpenSSL `AES_ige_encrypt`, `:634`). IGE **requires
  a 16-byte block multiple** and asserts `OPENSSL_assert((length % 16) == 0)` →
  `OPENSSL_die()` → `abort()`. A 4-aligned-but-not-16-aligned length (24, 28, 40, …)
  passes the gate and aborts the process.
- **Reachability:** pre-auth. Attacker drives the plaintext handshake to `Stage::WaitingDH`
  (reply to `req_pq` with a factorable `pq` + a known embedded RSA fingerprint), then sends
  `server_DH_params_ok` echoing the cleartext `nonce`/`server_nonce` with a 24-byte
  `encrypted_answer`. MITM / malicious DC / malicious proxy all qualify.
- **Impact:** deterministic remote **DoS** (SIGABRT). Clean abort, not corruption
  (`decBuffer` is sized to `encDHLen`, so no OOB even absent the assert). No write
  primitive, no info leak. **Medium–High** (pre-auth remote crash, fires on ~half of all
  malformed lengths, tunable to every connect).
- **Demonstrated:** `bash repro.sh` → buggy gate accepts len 24 → `AES_ige_encrypt` →
  `Aborted (core dumped)` (exit 134); proposed `& 0x0F` gate rejects it (exit 0). Verified
  against the OpenSSL the build links (3.x; assertion unchanged since 0.9.8).
- **Fix (one token):** widen the alignment gate `& 0x03` → `& 0x0F` at `:605`. Legit
  hellos are always 16-aligned, so no valid handshake is affected. Full detail +
  regression test in `FINDING-mtproto-3-ige-align.md`.

Why prior passes missed it: they verified the *buffer bounds* of `dhParamsAnswered`
(`decBuffer` sizing, `end = from + (encDHBufLen-5)` in-range) and correctly called those
clean — but did not check the *library precondition* on the decrypt length. The bug is
not an OOB; it is an unhandled `abort()` one call earlier.

---

## Consolidated findings (whole thread)

| ID | Defect | File | Class | Reach | Status |
|----|--------|------|-------|-------|--------|
| **MTPROTO-1** | not-secure resp byte-len used as prime count | `connection_abstract.cpp:149` | OOB heap **read** | pre-auth, MITM/proxy | **Fixed + committed** (`4467e0cbf1`), re-verified |
| **MTPROTO-2** | FakeTLS Server Hello short-length | `details/mtproto_tls_socket.cpp:763` | OOB read + zero-**write** / abort-DoS | pre-auth, FakeTLS/proxy | **Fixed + committed** (`4467e0cbf1`), re-verified |
| **MTPROTO-3** | DH `encrypted_answer` not 16-aligned → IGE abort | `details/mtproto_dc_key_creator.cpp:605` | `abort()` **DoS** | pre-auth, MITM/proxy | **NEW — open**, PoC'd |
| #3 | RSA `decrypt` leading-zero shift offset | `details/mtproto_rsa_public_key.cpp:158` | size_t underflow → abort / OOB write | pre-auth config, **dead on OpenSSL3/BoringSSL** | Open (latent), re-confirmed |
| #4 | unbounded recursion `handleOneReceived` (container/gzip nesting) | `session_private.cpp:1469,1524` | stack-overflow DoS | **post-auth**, malicious DC | Open, re-confirmed present |
| #5 | unbounded recursion TL `RichText` deserialization | generated `scheme.cpp` + `tl_boxed.h` | stack-overflow DoS | post-auth server data | Open, fuzzer-found (prior) |
| F-1 | pinned FFmpeg `n6.1.1` stale (~930 behind; 25 mem-safety fixes in reachable demuxers) | group `VideoStreamingPart.cpp` → ffmpeg | mostly DoS, some heap-OOB | group-call broadcaster | Open, stale-dep |
| O-1 | `Version0` `0x7F` length unclamped (no `kPacketSizeMax`) | `connection_tcp.cpp:118` | ~64 MB alloc-DoS | MITM/proxy | Open, low |
| DoS-a | `ungzip` output uncapped (zip-bomb) | `session_private.cpp:~2025` | memory-DoS | malicious DC | Accepted class; cap recommended |
| DoS-b | `tl::vector_type::read` unbounded `count` alloc | `lib_tl/.../tl_basic_types.h` | alloc-DoS | any TL sender | Accepted class (previously cleared) |

Net-new this pass: **1 (MTPROTO-3)**. Everything else is a re-verification of prior
work against the current tree.

---

## Re-verification of the committed fixes (both correct, in tree at HEAD)

- **MTPROTO-1** (`connection_abstract.cpp:149`): the return is
  `gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime))` — byte length converted to a
  prime count, so the span end matches the validated body. Both consumers
  (`readPQFakeReply` `:166`; `DcKeyCreator::readNotSecureResponse` `dc_key_creator.cpp:435`)
  fence the TL read on `answer.size()`; one conversion covers every not-secure message.
  Correct and complete.
- **MTPROTO-2** (`mtproto_tls_socket.cpp:763`): the guard
  `if (_serverHelloLength < kServerHelloDigestPosition + kHelloDigestLength) { … handleError(); return; }`
  precedes the digest subspan. The rest of the hello state machine
  (`readHello`/`checkHelloParts12`/`checkHelloParts34`, `:691-753`) is bounded by
  `requiredHelloPartReady()` (`_incoming.size() >= 32 + _serverHelloLength`) at each step,
  and every path into `checkHelloDigest` re-runs that gate. This was the only unguarded
  subspan. Correct and complete.

---

## Re-confirmation of the open findings (still present at HEAD)

- **#3 RSA decrypt offset** (`mtproto_rsa_public_key.cpp:156-160`): still
  `subspan(zeroBytes - res, res)` where the sibling `encrypt` (`:138-142`) correctly uses
  `subspan(zeroBytes, res)`. `zeroBytes - res == 256 - 2*res` goes negative for `res > 128`
  → `size_t` underflow → gsl abort or wild `bytes::move`. **Dead on OpenSSL 3.x / BoringSSL**
  (`RSA_public_decrypt(RSA_NO_PADDING)` returns the full 256, so `zeroBytes == 0` and the
  branch is skipped). Latent correctness bug; fix = mirror `encrypt`.
- **#4 handleOneReceived recursion** (`session_private.cpp:1469` gzip_packed, `:1524`
  msg_container): both self-calls remain, no depth counter in the signature or `OuterInfo`.
  Post-auth (needs session key → malicious DC). Fix = thread a small depth bound (≤8).
- **O-1 Version0 0x7F** (`connection_tcp.cpp:118`): still
  `return (ints >= 0x7F) ? (int(ints << 2) + 4) : kInvalidSize;` — a 24-bit `ints` yields
  up to ~64 MB with no `kPacketSizeMax` clamp (VersionD clamps; `:212`). Bounded alloc-DoS,
  MITM/proxy. One-line clamp for parity.
- **F-1 stale FFmpeg / #5 RichText recursion**: unchanged from prior reports; not
  re-derived this pass (see `../mtproto-tgcalls-audit/SESSION_FINDINGS.md`).

---

## Seams examined **this pass** and swept clean (evidence)

Focus was the seams prior passes marked *not yet examined* + the AES-precondition class.

- **Whole AES-IGE call-site audit** (systematic): the only attacker-facing IGE **decrypt**
  with a length not forced to 16-byte alignment is `dc_key_creator.cpp:634` (= MTPROTO-3).
  The post-auth message decrypt masks alignment (`session_private.cpp:1303`,
  `& ~0x03U` → whole 4-prime/16-byte blocks) and is safe; every IGE **encrypt** site
  (`dc_key_creator.cpp:293,355`, `dc_key_binder.cpp:65`, `session_private.cpp:2690`) uses
  client-controlled, always-16-aligned lengths.
- **Temp-key binder** `details/mtproto_dc_key_binder.cpp:107` `handleResponse`: reads
  `response[0]` after `Expects(!response.isEmpty())`, then a bounds-checked
  `MTPRpcError::read(from, end)`. No raw indexing. Clean. (`EncryptBindAuthKeyInner` is
  send-side / client-controlled.)
- **DH inner-data decrypt** `dc_key_creator.cpp:623-664` (apart from MTPROTO-3):
  `decBuffer` sized to `encDHBufLen` primes; `&decBuffer[5]` valid (`encDHBufLen ≥ 6`);
  `end = from + (encDHBufLen-5)` is one-past-end; the post-read SHA1 subspan
  `subspan(20, (to-from)*4)` stays `≤ decBuffer` since `to ≤ end`. In-bounds.
- **FakeTLS hello state machine** `mtproto_tls_socket.cpp:691-793`: each `ReadPartLength`
  / `CheckPart` subspan is gated by `requiredHelloPartReady()`; `part1Offset == 0`; the
  digest is now length-guarded (MTPROTO-2). Clean post-fix.
- **Censorship-config decrypt** `special_config_request.cpp:422-486` `decryptSimpleConfig`:
  blob size-gated to exactly base64 344 → 256 bytes; uses **AES-CBC** (not IGE — no block
  assert), payload always 224 bytes (16-aligned); `realLength` validated `(0, dataSize]`,
  `%4`, with a trailing-length cross-check. Clean, and **not** exposed to the IGE class.
- **tgcalls v2 reflector** `v2/ReflectorPort.cpp:664-777` `HandleIncomingPacket`: `size ≥ 16`
  gate; peer-tag `memcpy(…,16)` guarded; the relay data path requires `size > 24` before
  reading the big-endian `dataSize` at offset 20, then rejects `dataSize > size - 24`
  (no `size_t` underflow — `size > 24` holds) before `DispatchPacket(data+24, dataSize)`.
  Fully bounded.
- **tgcalls ffmpeg AVIO glue** `group/AVIOContextImpl.cpp:15-46`: read clamps
  `min(bufferSize, size - pos)` (and `< 0 → 0`); seek clamps to `[0, size]`. Safe wrapper —
  the residual is downstream ffmpeg only (F-1).

## tgcalls / lib_webrtc version posture (re-checked)

- tgcalls submodule = `616810f1` (2026-04-15) — recent; matches prior. WebRTC/tg_owt pin
  tracks upstream master (not stale). The productive edge remains the tdesktop glue seams
  (all bounded above) and the **stale FFmpeg** pin (F-1), not out-fuzzing OSS-Fuzz on
  current WebRTC internals.

---

## Fuzzing campaign (this pass — 4 harnesses, a few cores)

Complementing the static trace, four libFuzzer+ASan campaigns were run in parallel
(`-fork`, 6+4+2+2 workers on 16 cores). Two reuse prior harnesses; two are the MTProto
TL deserializer over the real generated `scheme.cpp` (4.5 MB), which is the codegen'd half
of the "post-deserialization interpretation" surface the brief prioritizes.

| harness | target | file | forks |
|---------|--------|------|-------|
| `fuzz_tl` | 32 **post-auth** server-push boxed types (Updates, Page, RichText, PageBlock, WebPage, Difference, …) | `mtproto-tgcalls-review/fuzz/fuzz_tl.cpp` | 6 |
| `fuzz_tl_pre` (**new**) | **pre-auth handshake** types (`ResPQ`, `Server_DH_Params`, `Server_DH_inner_data`, `Set_client_DH_params_answer`, `Client_DH_Inner_Data`, `P_Q_inner_data`) — the MTPROTO-1/3 surface | `mtproto-tgcalls-review/fuzz/fuzz_tl_pre.cpp` | 4 |
| `fuzz_videostream` | tgcalls group-call `consumeVideoStreamInfo` container header | `tgcalls-mtproto-audit/fuzz_videostream.cpp` | 2 |
| `fuzz_mtp_length` | MTProto TCP `Version0`/`VersionD` length parsers (proxy/MITM seam) | `tgcalls-mtproto-audit/fuzz_mtp_length.cpp` | 2 |

Each fuzz input is copied into an aligned heap `mtpPrime` buffer bracketed by ASan
red-zones, so any past-`end` read in the generated `read(from, end)` path is caught;
`Expects`/`Assert` route through `base::assertion::fail` (null-deref → ASan). The TL
harnesses compile against a FUZZ-ONLY `override/tl/tl_basic_types.h` that bounds the
`vector_type::read` `count` to the remaining payload — this steps past the known
unbounded-`count` alloc-DoS (DoS-b) so the deserializers underneath actually get exercised
(and it is the shape of the real fix that surface should adopt).

**Results (~460M executions total, a few cores):**

| harness | executions | cov | crashes | verdict |
|---------|-----------:|----:|--------:|---------|
| `fuzz_tl` (post-auth) | ~54M | 13359 | **38** | all = the known **#5 TL-recursion stack-overflow** (see below); **0 heap-corruption** |
| `fuzz_tl_pre` (pre-auth) | ~23M | 1308 | 0 | pre-auth handshake deserializers bounds-safe |
| `fuzz_videostream` | ~120M | 176 | 0 | group-call header parser bounds-safe |
| `fuzz_mtp_length` | ~266M | 15 | 0 | TCP length parsers bounds-safe (alloc-DoS only) |

- The 38 `fuzz_tl` crashes are **not** memory corruption — every one is the unbounded-recursion
  stack-overflow (finding **#5**). Confirmed by direct repro: a boxed `RichText` of ~200k
  nested `textBold#6724abc4` constructors → `AddressSanitizer: stack-overflow` inside the
  recursive `read()` chain (PoC `poc_richtext_recursion.bin`, exit via ASan). The seed corpus
  (carried from prior campaigns) already contained these deep inputs, so they reproduce on load;
  the pre-auth harness started from an empty corpus and did not synthesize them in 8 min → 0.
  This is a clean SIGSEGV via the stack guard page = **DoS**, post-auth, malicious-server-gated —
  same class as #4. It masks deeper post-auth coverage; a recursion-depth override on the harness
  (like the existing `count` override for the alloc-DoS) would let the fuzzer explore past it
  (next-step, not done this pass per the "don't over-invest" scoping).
- **Net new memory-corruption from fuzzing this pass: 0.** The dynamic result agrees with the
  static trace: the generated TL read path and the tgcalls/MTProto length parsers are bounds-safe;
  the residual is the two known recursion DoS (#4/#5) and the accepted alloc/decompress-DoS.
  MTPROTO-3 (the new finding) lives in the **AES layer above** the TL reads, so it is out of these
  harnesses' scope and was found + PoC'd by static tracing instead.

## Reproduction (MTPROTO-3)

```
cd dev/mtproto-webrtc-deep
bash repro.sh
#  len=24  -> OpenSSL "assertion failed: (length % AES_BLOCK_SIZE) == 0" -> Aborted (134)
#  buggy gate accepts len 24 -> SIGABRT ;  fixed gate (& 0x0F) rejects -> exit 0
```

Needs a system OpenSSL with dev headers (`-lcrypto`). The abort is in libcrypto and is
independent of build type (OpenSSL's assert is not `NDEBUG`-gated).

---

## Recommended actions before release (priority order)

1. **Fix MTPROTO-3** — `mtproto_dc_key_creator.cpp:605` `& 0x03` → `& 0x0F`. One token;
   add the short-`encrypted_answer` regression test. *(Optional hardening: reject
   `len % 16 != 0` inside `aesIgeEncryptRaw`/`aesIgeDecryptRaw` so no future caller can
   reintroduce the abort.)*
2. **Land regression tests for MTPROTO-1 / MTPROTO-2** (already fixed) so they don't
   regress: oversized/short not-secure response; short FakeTLS hello.
3. **Fix #3** defensively (`subspan(zeroBytes, res)`), and **bound #4** (recursion-depth
   guard in `handleOneReceived`) and **#5** (TL parse-depth guard).
4. **Cap `ungzip` output** (mirror the tgcalls v2 2 MiB gunzip cap) and **clamp O-1**.
5. **Bump the pinned FFmpeg** off `n6.1.1` and/or allowlist the group-call `container`
   string (F-1).

## Key file:line references (verified this pass)

- `mtproto/details/mtproto_dc_key_creator.cpp:605` — MTPROTO-3 gate (`encDHLen & 0x03`);
  `:634` — the `aesIgeDecryptRaw(encDHLen)` call.
- `mtproto/mtproto_auth_key.cpp:161-169` — `aesIgeDecryptRaw` → `AES_ige_encrypt` (no
  `%16` guard).
- `mtproto/session_private.cpp:1303` — `encryptedIntsCount = (…) & ~0x03U` (16-aligned,
  safe); `:1469,1524` — `handleOneReceived` recursion (#4).
- `mtproto/connection_abstract.cpp:149` — MTPROTO-1 fix (committed).
- `mtproto/details/mtproto_tls_socket.cpp:763` — MTPROTO-2 fix (committed); `:691-753` hello
  state machine.
- `mtproto/details/mtproto_dc_key_binder.cpp:107` — `handleResponse` (clean).
- `mtproto/details/mtproto_rsa_public_key.cpp:158` — `decrypt` offset (#3, latent).
- `mtproto/special_config_request.cpp:455` — `AES_cbc_encrypt` on fixed 224-byte payload
  (clean).
- `mtproto/connection_tcp.cpp:118` — `Version0` `0x7F` unclamped (O-1).
- `ThirdParty/tgcalls/tgcalls/v2/ReflectorPort.cpp:664-777` — relay packet parse (bounded).
- `ThirdParty/tgcalls/tgcalls/group/AVIOContextImpl.cpp:15-46` — ffmpeg AVIO glue (bounded).
