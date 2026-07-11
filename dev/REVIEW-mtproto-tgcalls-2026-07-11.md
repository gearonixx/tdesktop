# Pre-release robustness review — MTProto transport & tgcalls glue (2026-07-11)

Scope: memory-safety / robustness of network-facing byte handling in
`Telegram/SourceFiles/mtproto/` and `Telegram/ThirdParty/tgcalls/`.
Method: static tracing of the read/deserialization paths (bytes → gate → handler),
confirming reachability before claiming impact. No new harness stood up — the two
areas were cleared by targeted tracing (matching the task's "tracing beats fuzzer here"
guidance), and tgcalls was version-scored first.

## Summary

Two genuine memory-safety defects were already found and fixed in the working tree
(uncommitted). This pass **re-confirmed** them and did a fresh sweep of the remaining
read paths; no additional memory-corruption defect was found. Residual risk is
alloc/decompress-DoS reachable only from a malicious/MITM server — the known,
accepted class.

## Confirmed findings (already fixed in working tree)

### 1. Pre-auth OOB heap read in `parseNotSecureResponse` — FIXED
`connection_abstract.cpp:141`. `answerLen` is a **byte** length (written as `len << 2`
in `prepareNotSecurePacket`) but was passed to `gsl::make_span(answer + 5, answerLen)`
as an **element** (mtpPrime = int32) count, yielding a span up to 4× the real data.
Callers use `span.size()` as the TL read boundary, so the deserializer could read
~3×len primes past the packet allocation.
- Reachability: **pre-auth**. Callers are `readPQFakeReply` (TCP `connection_tcp.cpp:602`,
  HTTP `connection_http.cpp:210`) and `DcKeyCreator` (`mtproto_dc_key_creator.cpp:455`) —
  all during the unauthenticated handshake. Trigger: malicious/MITM server or MTProto proxy.
- Fix: `answerLen / sizeof(mtpPrime)` so the span length is a prime count. Correct.
- Severity: OOB **read** → info-leak / crash. No write primitive. Upstream since ~2018.

### 2. FakeTLS Server Hello length DoS — FIXED
`details/mtproto_tls_socket.cpp:757` (`checkHelloDigest`). The Server Hello part2/part4
length fields are fully attacker-controlled, so `_serverHelloLength` can be smaller than
the fixed digest region. The subsequent `subspan(0, kHelloDigestLength + _serverHelloLength)`
and the zero-write of the digest could reach past `_incoming`.
- Reachability: pre-auth, FakeTLS transport with a malicious/MITM endpoint.
- Fix: reject when `_serverHelloLength < kServerHelloDigestPosition + kHelloDigestLength`. Correct.

## New finding (latent / defense-in-depth) — RSA decrypt leading-zero compensation

`details/mtproto_rsa_public_key.cpp:156-159`, `RSAPublicKey::Private::decrypt`. When
`RSA_public_decrypt(..., RSA_NO_PADDING)` returns fewer than `kDecryptSize` (256) bytes,
the code left-shifts the recovered bytes into place. It computes the destination offset as
`zeroBytes - res` instead of `zeroBytes`:

```cpp
} else if (auto zeroBytes = kDecryptSize - res) {           // res in [0,255]
    auto resultBytes = gsl::make_span(result);
    bytes::move(resultBytes.subspan(zeroBytes - res, res), resultBytes.subspan(0, res));
    bytes::set_with_const(resultBytes.subspan(0, zeroBytes - res), gsl::byte{});
```

The sibling `encrypt()` (`:140`) does the correct thing: `subspan(zeroBytes, res)`.
`zeroBytes - res == 256 - 2*res`, which is **negative for res > 128** (e.g. res=255 → -254).
`subspan`'s offset is `size_t`, so a negative int becomes an enormous offset →
`gsl` `Expects(offset <= size())` aborts (DoS), or, with contracts disabled, a wild
`bytes::move` (heap OOB write). For res in [1,128] it silently writes to the wrong offset
(garbage plaintext → later digest mismatch).

- Caller: `special_config_request.cpp:448` `decryptSimpleConfig` — the pre-auth
  censorship-circumvention config fetch (DoH TXT / Firebase / Firestore). The 256-byte
  RSA input is attacker-influenceable if the fallback config response is tampered.
- **Reachability: effectively unreachable today.** On OpenSSL 1.1+/3.x and BoringSSL,
  `RSA_public_decrypt(RSA_NO_PADDING)` returns the full modulus size (256) — the result is
  left-zero-padded by `RSA_padding_check_none`, which returns `num`. So `res == 256`,
  `zeroBytes == 0`, and the buggy `else if` block is never entered. The defect only bites a
  crypto backend that returns a leading-zero-*stripped* length (older/alternate libs).
- Severity: latent. Recommend either matching `encrypt` (`subspan(zeroBytes, res)`) or
  asserting `res == kDecryptSize`, as defense-in-depth. Not a shippable vuln on the
  current crypto backends.

## Paths swept clean this pass (evidence)

**MTProto**
- TCP framing `connection_tcp.cpp`: `readPacketLength` caps every variant
  (`kPacketSizeMax`, `ints<<2` bounded, `kInvalidSize` on junk); `readPacket`/`parsePacket`
  assert `size <= bytes.size()` and non-empty. Length handling is guarded.
- Encrypted-message intake `session_private.cpp:1275` `handleReceived`: `intsCount` range-checked
  `[kMinimalIntsCount, kMaxMessageLength/4]`; `messageLength` bounded and `%4`; `end` derived
  within the decrypted buffer; mandatory padding (`paddingSize ≥ 12`) guarantees the
  `rpc_result` one-prime read past `end` (`:1848`) stays inside the allocation. msg_key SHA256
  verified before interpretation.
- Container `:1472` / rpc_result `:1824`: per-message `from+4 >= end` and `otherEnd > end`
  guards; `bytes.v` checked `≥4` and `%4`. No unbounded index.
- `ungzip` `:2025`: **decompressed size uncapped** → zip-bomb memory-DoS, but bounded by
  malicious-server reachability and input `≤ kMaxMessageLength`; same code ships upstream.
  Alloc-DoS class, not corruption.
- DH key exchange `mtproto_dc_key_creator.cpp:587` `dhParamsAnswered`: `encDHLen` checked
  `%4` and `encDHBufLen ≥ 6`; `end = from + (encDHBufLen-5)` is exactly one-past the decrypted
  buffer; SHA1 subspan bounded by consumed `(to-from)`. Clean.
- TL primitives `lib_tl/tl/tl_basic_types.h`: `vector_type::read` does
  `QVector<T>(count, T())` from an unbounded attacker `count` before reading elements →
  the known **alloc-DoS** (already cleared). `string_type::read` bounds every branch via
  `HasBytes`; `Reader<mtpPrime>::Has/HasBytes/Get` all check `end - from`. No OOB.

- DoH / special-config fetch (`mtproto_domain_resolver.cpp`, `special_config_request.cpp`):
  DNS JSON parsed via Qt `QJsonDocument` (bounds-safe); the config blob is size-gated
  (base64 exactly 344 → 256 bytes) and every subspan in `decryptSimpleConfig` is derived
  from those fixed sizes; `realLength` validated `(0, dataSize]` and `%4` before the TL read,
  with a trailing-length cross-check. Clean **apart from** the RSA-decrypt latent bug above.
- HTTP transport (`connection_http.cpp`): Qt owns HTTP framing/Content-Length; post-read
  guards `size % 4` and `size >= 8`, then the (already-fixed) `readPQFakeReply` path.

**tgcalls glue** (submodule `616810f`, 2026-04-15 — recent; upstream CVE surface unlikely
reachable-and-unfixed)
- `EncryptedConnection.cpp`: intake gated `size ∈ [21, 128KiB]`; msgKey SHA256 verified
  before parse; `processPacket`/`processRawPacket` loop always has ≥1 byte at `*reader.Data()`
  and requires ≥5 before continuing (consuming 4), so no empty-reader deref; each message
  consumes ≥1 byte so no infinite loop; replay/counter window bounded.
- `Message.cpp`: string length capped `< kMaxStringLength` (64KiB) and bounds-checked;
  vector/format counts are `uint8` (≤255); `Deserialize(CopyOnWriteBuffer)` checks
  `from.Length() < length` before `AppendData`; raw-message length capped `≤ 1MiB` with
  `ReadBytes` bounds check. Single-packet `uint16_t(from.Length())` truncation is
  harmless (truncated ≤ actual; over-long single packets error out, no OOB).
- `NetworkManager.cpp:347` entry gate goes straight to `handleIncomingPacket`, which
  rejects `size < 21` before dereferencing bytes; `CryptoHelper.cpp` is fixed-size only.
- Group-call `AudioStreamingPartInternal.cpp` custom header parser: `readInt32` bounds-checks
  `offset + 4 > length` before each `memcpy`; the `count`-driven loop bails via `readInt32`
  when data is exhausted, so no unbounded index/alloc. Media bytes then flow into ffmpeg
  (out of scope: heavily fuzzed upstream).

## Bottom line

Net-new **memory-corruption** defects reachable on current crypto backends this pass: **0**.
One latent defect found (RSA `decrypt` offset math, `mtproto_rsa_public_key.cpp:158`) —
real code bug, but dead on OpenSSL 3.x / BoringSSL because `RSA_public_decrypt` returns the
full 256-byte length; worth a defensive fix. The two genuine OOB bugs (both pre-auth, both
MITM/malicious-server gated) were already patched in the working tree and verified correct
here. Everything else on the read path is either properly bounded or falls in the
previously-accepted alloc/decompress-DoS residual.

Recommended actions:
1. Commit the two working-tree fixes with regression tests (short/oversized not-secure
   response; short FakeTLS hello).
2. Fix `RSAPublicKey::Private::decrypt` to use `subspan(zeroBytes, res)` (match `encrypt`)
   or assert `res == kDecryptSize`.

---

## Addendum — independent re-verification (2026-07-11, second pass)

A fresh, independent trace re-derived M-1 and M-2 from scratch and reproduced the prior
conclusions. Two precisions worth recording:

- **M-1 fix location is optimal.** Both consumer families fence the TL read on the span
  size — `readPQFakeReply` (`connection_abstract.cpp:166`) and the
  `DcKeyCreator::readNotSecureResponse<T>` template (`dc_key_creator.cpp:435`, used for
  `Req_pq` / `Req_DH_params` / `Set_client_DH_params`). Converting bytes→primes once inside
  `parseNotSecureResponse` covers every not-secure handshake message; no caller-side change
  needed.

- **M-2 is a *deterministic* crash, not silent corruption, in the shipped build.** The
  failing slice is `fulldata.subspan(43, 32)` on a 48-byte span. GSL `make_subspan`
  (`ThirdParty/GSL/include/gsl/span:760`) runs `Expects(size() - offset >= count)` =
  `Expects(5 >= 32)`. No `GSL_THROW_ON_CONTRACT_VIOLATION` /
  `GSL_UNENFORCED_ON_CONTRACT_VIOLATION` macro is defined anywhere in the build, so
  `Expects` resolves to `std::terminate()` — the process aborts *before* the digest
  zero-write executes. So pre-fix behavior is a guaranteed unauthenticated remote
  termination (DoS); the "zero-write past `_incoming`" only materializes as an actual OOB
  heap write in a build that compiles GSL contracts out. Either way the `>= 43` guard is
  the correct fix and severity stays Medium (pre-auth DoS).

Everything else in the prior report was re-checked and stands. Net-new
memory-corruption defects this second pass: 0.
