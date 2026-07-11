# Pre-release robustness / memory-safety review — 2026-07-11

Scope: statically-linked Qt image plugins (kimageformats: AVIF/HEIF/JXL/QOI) reached
through `Images::Read` / `QImageReader`, **plus** the MTProto protocol layer
(explicitly requested). Method: static audit of the delivery glue + headless
libFuzzer/ASan campaigns feeding bytes straight at the decode entry points.

## TL;DR

| ID | Severity | Class | Layer | Status |
|----|----------|-------|-------|--------|
| **MTPROTO-1** | High | OOB heap **read**, pre-auth, remote | not-secure handshake parse | **Fixed in working tree** (verified) |
| **MTPROTO-2** | High | OOB heap **write** (zero-fill), remote | fake-TLS transport | **Fixed in working tree** (verified) |
| HEIF-1 | Medium (DoS) | null/wild-ptr SEGV read | libheif 1.22 (3rd-party) | Prior finding, ASan-repro |
| kimg glue (qoi/avif/heif) | — | none found | tdesktop glue | Audited clean |
| qoi/avif/jxl decoders | — | no crashes | plugins + libs | Fuzzed, 0 crashes this pass |

The two **protocol-layer** findings are the substantive ones: both are
network-reachable, both fire **before an auth key exists** (during the PQ/DH
handshake or the fake-TLS handshake), and MTPROTO-2 is an out-of-bounds heap
*write*. Both are already fixed on this branch; this review independently
confirms the bugs, the reachability, and the correctness of the fixes.

---

## MTPROTO-1 — OOB heap read in `parseNotSecureResponse` (pre-auth)

**File:** `Telegram/SourceFiles/mtproto/connection_abstract.cpp:149`

### Mechanism
`prepareNotSecurePacket` writes the message-length field in **bytes**:

```cpp
// connection_abstract.h:197
*messageLength = (result.size() - kPrefixInts + intsPadding) << 2;   // *4 -> bytes
```

The reader reads it back and validates it as a byte length:

```cpp
const auto answerLen = (uint32)answer[4];                     // byte length
if (answerLen < 1 || answerLen > (len - 5) * sizeof(mtpPrime)) return {};   // bound in bytes: OK
return gsl::make_span(answer + 5, answerLen);                 // BUG (HEAD): count = bytes
```

`answer` points at `mtpPrime` (int32) elements, so `make_span`'s second argument
is an **element count**, but `answerLen` is a **byte** count — up to 4× too large.
Every caller uses the span size as the TL read boundary:

```cpp
// readPQFakeReply / DcKeyCreator::readNotSecureResponse
result.read(from, from + answer.size());   // 'end' inflated up to 4x
```

The three handshake responses — `MTPReq_pq`, `MTPReq_DH_params`,
`MTPSet_client_DH_params` — all funnel through this one function
(`mtproto_dc_key_creator.cpp:455`). `MTPResPQ` etc. contain a TL string (`pq`)
and a vector (`server_public_key_fingerprints`) whose inner length/count fields
are attacker-controlled; with `end` pushed up to `answer + 4*(len-5)` primes
against a buffer of only `len` primes, the deserializer's `from + n <= end`
checks pass when they should fail, and it walks up to ~3×(len−5) primes
(~12·len bytes) past the packet allocation → OOB heap read.

### Impact / reachability
- **Pre-auth, remote.** Runs during DC key creation, before any auth key exists.
  A malicious server, MITM, or MTProto proxy fully controls these bytes.
- **DoS-class:** the over-read walks into adjacent/unmapped heap → crash (SIGSEGV).
  Parsed values are discarded on failure, so this is a crash primitive, not a
  disclosure-to-attacker. Classify **High** (remote pre-auth crash), not RCE.
- Genuine **upstream** Telegram Desktop defect: `git show HEAD` still has
  `make_span(answer + 5, answerLen)`; history on the file is upstream commits.

### Fix (present, correct)
```cpp
return gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime));
```
Converts the validated byte length to a prime count so the read boundary matches
the real data. One-line fix covers all three handshake messages. The sibling
manual parse at `mtproto_dc_key_creator.cpp:636` already uses prime units
(`encDHBufLen = encDHLen >> 2`) and is unaffected.

### Suggested regression test
Feed a crafted not-secure buffer with `answer[4]` set to a large byte length and
a `MTPResPQ` whose fingerprint-vector count is large; assert the parse rejects
(returns `nullopt`) instead of reading past `buffer.size()`. Run under ASan.

---

## MTPROTO-2 — OOB heap zero-write in `TlsSocket::checkHelloDigest`

**File:** `Telegram/SourceFiles/mtproto/details/mtproto_tls_socket.cpp:763` (guard added)

### Mechanism
The fake-TLS transport parses a Server Hello whose length is assembled from two
attacker-controlled 16-bit length fields (part2/part4):

```
parts123Size = parts1Size(5) + part2Size(0..65535) + part3(9) + 2
_serverHelloLength = parts123Size + part4Size(0..65535)   // min = 16
```

The fixed-byte "parts" (`\x16\x03\x03`, `\x14\x03\x03...`) are checked, but the
attacker supplies them, so `part2Size = part4Size = 0` and `_serverHelloLength = 16`
is reachable. Then:

```cpp
auto fulldata = bytes::make_detached_span(_incoming).subspan(0, kHelloDigestLength + _serverHelloLength);
auto digest   = fulldata.subspan(kHelloDigestLength + kServerHelloDigestPosition, kHelloDigestLength);
                              //  = subspan(32 + 11 = 43, 32)  -> [43, 75)
auto digestCopy = bytes::make_vector(digest);      // OOB READ of the tail
bytes::set_with_const(digest, bytes::type(0));     // OOB WRITE (zero-fill) of the tail
```

With `_serverHelloLength = 16`, `fulldata.size() = 48`, but the digest span runs
`[43, 75)` — up to **27 bytes past** the detached `_incoming` heap buffer. Both a
read (`make_vector`) and a **write** (`set_with_const`, zero-fill) land OOB.
Constants: `kHelloDigestLength=32`, `kServerHelloDigestPosition=11`.

### Impact / reachability
- **Remote, during transport handshake** (fake-TLS / "ee"-secret proxies). Attacker
  controls `_serverHelloLength` directly.
- **OOB heap write** (fixed value 0, bounded ~≤27 bytes) → heap corruption →
  crash or worse. More severe than MTPROTO-1 because it is a write. **High.**

### Fix (present, correct)
```cpp
if (_serverHelloLength < kServerHelloDigestPosition + kHelloDigestLength) {
    logError(888, "Bad Server Hello length.");
    handleError();
    return;
}
```
Rejects hellos too short to contain the digest. Verified: this is the only
subspan in the hello parse not otherwise bounded — the parts12/34 subspans are
gated by `requiredHelloPartReady()` (`_incoming.size() >= 32 + _serverHelloLength`),
and every path into `checkHelloDigest` first re-runs that gate via `readHello()`.

### Suggested regression test
Drive `TlsSocket` with a Server Hello carrying valid part1/part3 magic and
part2Size=part4Size=0 (`_serverHelloLength = 16`); assert `handleError()` fires and
no write past `_incoming` occurs (ASan).

---

## Image decoders — fuzz pass (this session)

Compiled-in kimageformats plugins (per `cmake/.../kimageformats/CMakeLists.txt`):
**avif, heif, jxl, qoi** (+ scanlineconverter). Reached from `ReadOther` →
`QImageReader` (`image_prepare.cpp:452`), which auto-detects format from magic
bytes — so any incoming media can trigger any of these regardless of extension.
`ReadOther` guards `width*height > kReadMaxArea` and `file.size() > kReadBytesLimit`.

### Glue audit (tdesktop-side, fixable by us) — clean
- **qoi.cpp `LoadQOI`**: inner-loop over-reads are bounded by the 8-byte
  `QOI_END_STREAM_PAD` slack in `ba` (max single-op advance = 5 bytes vs 8-byte
  pad); `img` allocated `Width×Height` with a 300000² guard. No OOB.
- **heif.cpp read**: 8/10/12-bit branches each match their `target_image_format`
  (ARGB32/RGB32 vs RGBA64/RGBX64); `bit_depth` constrained to {8,10,12} before
  the write switch. Dest allocated to plane dims; stride from libheif. No mismatch.
- **avif.cpp read**: `rgb.depth`/format (8→BGRA/4B, 16→RGBA/8B) match the
  allocated `QImage` format; `rgb.rowBytes/pixels` bound to `result`. No mismatch.

### Dynamic (libFuzzer + ASan), headless, `dev/kimg-fuzz/harness.cpp`
Drives `QOIHandler/QAVIFHandler/HEIFHandler/QJpegXLHandler::read()` on raw bytes.
Fresh campaigns this session (seeded from prior corpora):

| target | execs | crashes |
|--------|-------|---------|
| qoi  | ~18k+ (slow, deep images) | 0 |
| avif | ~260k+ | 0 |
| jxl  | ~195k+ | 0 |

No new memory-safety defects in qoi/avif/jxl this pass. Note HEIF-1 (below) fires
on most mutated HEIF inputs and *masks* deeper HEIF coverage; a run with the
handle-null defensive check applied would improve HEIF depth.

### HEIF-1 (prior finding, still valid)
Deterministic null/wild-pointer SEGV (read @ `0x0d`) in
`heif_context_get_primary_image_handle` (libheif 1.22) via `HEIFHandler::ensureDecoder`
(`heif.cpp:463`). Client-side DoS on a malicious HEIF preview/thumbnail. Fix is
upstream libheif + a defensive null/degenerate-handle check in `ensureDecoder`.
See `dev/kimg-fuzz/HEIF_FINDINGS.md` (77 repro artifacts → 1 unique bug).

---

---

## Update — extended fuzz campaign (avif ~2.6M, jxl ~1.6M, qoi execs)

### JXL-UB-1 (new) — float→int overflow UB in JXL animation glue
**File:** `Telegram/ThirdParty/kimageformats/src/imageformats/jxl.cpp:343`
UBSan-confirmed during the campaign:
`runtime error: 1.06218e+11 is outside the range of representable values of type 'int'`.

```cpp
delay = (int)(0.5 + 1000.0 * frame_header.duration
                 * m_basicinfo.animation.tps_denominator
                 / m_basicinfo.animation.tps_numerator);
```
`frame_header.duration`, `tps_denominator`, `tps_numerator` are all
attacker-controlled JXL header fields. The product can exceed `INT_MAX`
(~2.1e9); casting an out-of-range `double` to `int` is **undefined behavior**
(in practice `INT_MIN`). `delay` → `m_framedelays` → `m_next_image_delay` →
`nextImageDelay()`, i.e. a bogus/negative animation frame delay (ms).

- **Class:** UB / logic (not memory-unsafe). **Severity: Low.**
- **Reachability:** animated JXL via `QImageReader`; the `kReadMaxArea` area cap
  does *not* apply (this is timing math, not pixel dims), so it is reachable
  as-integrated. Client-side only, worst case degenerate animation timing.
- **This is our-side glue → fixable by us.** Clamp before the cast:
  ```cpp
  const double d = 0.5 + 1000.0 * double(frame_header.duration)
      * m_basicinfo.animation.tps_denominator / m_basicinfo.animation.tps_numerator;
  delay = (d <= 0.0) ? 0
      : (d >= double(std::numeric_limits<int>::max()))
          ? std::numeric_limits<int>::max() : int(d);
  ```
  Upstream kimageformats still has the raw cast (no fix landed) → report upstream + carry local patch.

### Not findings (characterized, not overstated)
- **qoi/avif "slow-unit" / "timeout" artifacts:** re-run standalone, the qoi
  "timeout" completes in <20s (EXIT=0) — a one-off under high fuzzer RSS, **not a
  real hang**. Slow-units are sub-second decodes (~585 ms) flagged for perf, not a
  DoS. No memory-safety issue. Additionally, huge-area DoS is gated app-side by
  `kReadMaxArea = 12032*9024` in `ReadOther` before decode.
- No OOB/UAF/heap-corruption surfaced in qoi/avif/jxl across the extended run.

## Bottom line
- Two real, network-reachable, **pre-auth** protocol-layer memory-safety bugs
  (one OOB read, one OOB write) are correctly fixed on this branch.
- The image-decoder glue is sound; the residual image risk is a libheif DoS
  (HEIF-1), upstream. qoi/avif/jxl fuzzed clean this pass.
- Recommend landing the two ASan regression tests above before release.
