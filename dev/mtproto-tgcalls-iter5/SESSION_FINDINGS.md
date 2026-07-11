# MTProto transport + tgcalls/lib_webrtc — Session Findings (iteration #5)

Date: 2026-07-11. Branch: `fix/mtproto-preauth-oob` (HEAD `4467e0cbf1`).
Target: Telegram Desktop fork — the **network-facing byte handlers** that must be
memory-safe on malformed / adversarial input before release: **MTProto transport /
TL deserialization** (`Telegram/SourceFiles/mtproto/`) and **tgcalls / lib_webrtc**
call transport (`Telegram/ThirdParty/tgcalls/`, `Telegram/lib_webrtc/`).

This iteration is a **close-out pass** on top of iterations #1–#4. It:
1. verifies the three memory-safety hardening fixes now sitting in the working tree
   (MTPROTO-3 IGE alignment, container/gzip recursion cap, TCP length cap);
2. **lands the one remaining clearly-correct source fix** — the RSA `decrypt`
   leading-zero offset bug (#3) — matching the proven `encrypt()` sibling;
3. does the still-outstanding **tg_owt / WebRTC CVE-scoring** that the task called
   "the productive question" for tgcalls, and reports the honest (negative) result;
4. re-verifies the group-call custom parser seam left flagged in prior passes.

Honest threat model (unchanged, governs every severity below): both areas require a
**malicious/MITM server, a malicious MTProto proxy, or a negotiated/compromised
call** to reach. The pre-auth handshake bugs (MTPROTO-1/2/3) are the strongest — no
auth key needed. Everything post-auth needs the session `auth_key`, i.e. a
malicious/compromised DC, not a passive MITM. The realistic ceiling for every
residual item is **DoS / remote crash**, not remote memory corruption or RCE.

Prior long-form reports this builds on (kept for their reasoning and PoCs):
- `../REVIEW-robustness-2026-07-11.md`, `../REVIEW-mtproto-tgcalls-2026-07-11.md`
- `../mtproto-tgcalls-robustness/SESSION_FINDINGS.md` (consolidation, iter #1–#4)
- `../mtproto-webrtc-deep/SESSION_FINDINGS.md` + `FINDING-mtproto-3-ige-align.md`
  (MTPROTO-3 discovery + PoC), `../tgcalls-mtproto-audit/SESSION_FINDINGS.md`
  (fuzzers, `poc_respq_dos`, `poc_notsecure_oob`).

---

## TL;DR — status at end of this pass

| # | Defect | File | Class | Reach | Status |
|---|--------|------|-------|-------|--------|
| 1 | `parseNotSecureResponse` byte-len used as mtpPrime element count | `connection_abstract.cpp:141` | OOB heap **read** (CWE-125) | **pre-auth** server/MITM/proxy | **FIXED — committed @HEAD** |
| 2 | FakeTLS Server Hello short-length subspan | `details/mtproto_tls_socket.cpp:757` | OOB read + zero-**write** past `_incoming` | **pre-auth** FakeTLS | **FIXED — committed @HEAD** |
| 3 | RSA `decrypt` leading-zero shift offset `zeroBytes - res` | `details/mtproto_rsa_public_key.cpp:158` | size_t underflow → abort / OOB **write** | pre-auth config fetch | **FIXED THIS PASS** (working tree) |
| MTPROTO-3 | `server_DH_params_ok` len 4-aligned but not 16-aligned → AES-IGE `OPENSSL_assert` abort | `details/mtproto_dc_key_creator.cpp:611` | **abort() DoS** (SIGABRT) | **pre-auth** server/MITM/proxy | **FIXED** (working tree), verified |
| 4 | Unbounded `handleOneReceived` recursion (`msg_container`/`gzip_packed`) | `session_private.cpp:1472` | **stack-overflow DoS** | post-auth, malicious DC | **FIXED** (working tree), verified |
| — | TCP `readPacketLength` Version0 uncapped size | `connection_tcp.cpp:122` | oversize-alloc hardening | pre-auth | **FIXED** (working tree), defense-in-depth |
| 5 | Unbounded TL recursion (`RichText` self-nesting; `PageBlock`) | `lib_tl/tl/tl_boxed.h` + generated `scheme.cpp` | **stack-overflow DoS** (SIGSEGV) | post-auth server data (webpage/IV) | **OPEN** — fuzzer-found + PoC'd; fix = TL parse-depth guard |
| — | `ungzip` no decompressed-size cap | `session_private.cpp:2042` | zip-bomb memory-DoS | post-auth, malicious DC | **OPEN** — accepted class; v2 storage path already caps @2 MiB |
| — | FFmpeg pin `n6.1.1` stale | `build/prepare/prepare.py` | stale-dep, mostly DoS | malicious group-call broadcaster | **OPEN** — bump pin |
| — | tg_owt / WebRTC pin `89df288` | `build/prepare/prepare.py:1700` | 3rd-party call transport | negotiated call | **No cheap reachable known-CVE** (see §4) |
| — | `tl::vector_type::read` unbounded `count` alloc | `lib_tl/tl/tl_basic_types.h` | alloc-DoS | malicious server | **Accepted class** |

**Net for release:** all **four** memory-safety / robustness bugs that are code-level
fixable in the main tree are now fixed (2 committed at HEAD + MTPROTO-3 + recursion +
RSA landed in the working tree). No shippable RCE was found in either area across five
iterations. What remains OPEN is: one TL-recursion stack-overflow DoS (#5, needs a
lib_tl parse-depth guard — the only item with a working PoC still unfixed), two accepted
alloc/decompress-DoS residuals, and a stale FFmpeg pin. None is remote memory
corruption; all need a malicious server or negotiated call.

---

## 1. Verification of the three working-tree hardening fixes

All three were re-derived from the current source this pass and confirmed correct.

### MTPROTO-3 — AES-IGE length alignment (`dc_key_creator.cpp:611`)
```cpp
if ((encDHLen & 0x0F) || encDHBufLen < 6) {   // was: (encDHLen & 0x03)
```
`encrypted_answer` from `server_DH_params_ok` is server-controlled. It is decrypted
with AES-256-**IGE** (`aesIgeDecryptRaw` → OpenSSL `AES_ige_encrypt`), which asserts
`OPENSSL_assert((length % 16) == 0)` → `OPENSSL_die()` → `abort()`. The old `& 0x03`
gate accepted lengths that are 4-aligned but not 16-aligned (24, 28, 40, …), so a
malicious server/MITM/proxy could deterministically abort the client **pre-auth**
during key creation. `& 0x0F` rejects those and implies the old 4-byte check. Legit
hellos are always 16-aligned, so no valid handshake is affected. Clean abort, not
corruption (`decBuffer` is sized to `encDHLen`), so severity is **Medium–High pre-auth
DoS**, not RCE. Full writeup + PoC: `../mtproto-webrtc-deep/FINDING-mtproto-3-ige-align.md`.

### #4 — container/gzip recursion depth cap (`session_private.cpp:60,1472`)
```cpp
constexpr auto kMaxHandleDepth = 16;
...
if (info.depth >= kMaxHandleDepth) {
    LOG(("Message Error: too deeply nested container/gzip_packed received."));
    return HandleResult::ParseError;
}
++info.depth;
```
`handleOneReceived` recurses through `mtpc_gzip_packed` (decompress → re-parse) and
`mtpc_msg_container` (per-message). A crafted deeply-nested message could exhaust the
stack (SIGSEGV). **Correctness of the counter verified:** `OuterInfo info` is passed
**by value**, so `++info.depth` increments independently down each recursion branch and
is *not* shared across sibling messages in a container loop — sibling breadth is
unbounded (that's fine; it's iterative and each sibling consumes ≥1 prime), only nesting
depth is capped. 16 is far above any legitimate nesting (a top-level container of
optionally-gzipped messages nests 2–3 deep). Post-auth (needs `auth_key`), so malicious
DC only. Correct.

### TCP length cap (`connection_tcp.cpp:118-122`)
```cpp
if (ints < 0x7F) { return kInvalidSize; }
const auto result = int(ints << 2) + 4;
return (result < kPacketSizeMax) ? result : kInvalidSize;
```
Version0's 3-byte length previously returned an uncapped size (up to ~64 MiB).
`readPacket` `Assert(size <= bytes.size())` already prevented an OOB read, so this is
**defense-in-depth**, not a memory-safety fix: it now matches the `VersionD` path
(`connection_tcp.cpp:217`, `value < kPacketSizeMax`) so an oversized length field is
rejected up front instead of driving a large speculative buffer. `int(ints << 2)` does
not overflow (`ints ≤ 0xFFFFFF`, so `ints<<2 ≤ 0x3FFFFFC`). Correct.

## 2. RSA `decrypt` leading-zero offset — FIXED THIS PASS

**File:** `details/mtproto_rsa_public_key.cpp:156-160`. Applied this pass:
```cpp
} else if (auto zeroBytes = kDecryptSize - res) {
    auto resultBytes = gsl::make_span(result);
    bytes::move(resultBytes.subspan(zeroBytes, res), resultBytes.subspan(0, res));   // was subspan(zeroBytes - res, res)
    bytes::set_with_const(resultBytes.subspan(0, zeroBytes), gsl::byte{});            // was subspan(0, zeroBytes - res)
}
```
When `RSA_public_decrypt(RSA_NO_PADDING)` returns `res < 256` (leading zeros stripped),
the recovered bytes must be right-aligned into the 256-byte result: move `[0,res)` to
`[zeroBytes, 256)` (`zeroBytes = 256 - res`) and zero-fill the front. The old code used
offset `zeroBytes - res = 256 - 2·res`, which is **negative for `res > 128`**; since
`subspan`'s offset is `size_t`, that becomes an enormous offset →
`Expects(offset <= size())` abort (DoS) or, with GSL contracts compiled out, a wild
`bytes::move` (heap OOB **write**). The `encrypt()` sibling (`:140`) already had the
correct `subspan(zeroBytes, res)`; this fix makes `decrypt` structurally identical.
`bytes::move` is `memmove` (`lib_base/base/bytes.h:97`), so the source/dest overlap for
`res > 128` is safe — exactly as `encrypt` already relies on.

- **Caller:** `special_config_request.cpp` `decryptSimpleConfig` — the pre-auth
  censorship-circumvention config fetch (DoH TXT / Firebase / Firestore).
- **Reachability today: effectively nil.** On OpenSSL 1.1+/3.x and BoringSSL,
  `RSA_public_decrypt(RSA_NO_PADDING)` returns the full modulus size (256, left
  zero-padded), so `res == 256`, `zeroBytes == 0`, and the buggy `else if` block never
  executes. The bug only bites a backend that returns a leading-zero-*stripped* length.
- **Why land it anyway:** it is a genuine, one-token correctness bug next to a
  crypto-input path, the fix is provably equivalent to the shipped-correct `encrypt`,
  and it removes a latent abort/OOB should the crypto backend ever change. Zero risk to
  current behavior (dead path today).

**Regression-test recipe** (standalone, no full build): call `decrypt` with an RSA key +
ciphertext under a stub `RSA_public_decrypt` returning `res = 200` with 56 leading zero
bytes; assert the 200 recovered bytes land right-aligned at `result[56..256)` and
`result[0..56)` are zero, with no abort. (Same shape as the existing `dev/` PoCs.)

## 3. Group-call custom parser seam — re-verified clean

`tgcalls/group/AudioStreamingPartInternal.cpp` `readInt32` / `parseChannelUpdates`
(the one hand-rolled byte parser in the group path, feeding the persistent Opus
decoder):
```cpp
static absl::optional<uint32_t> readInt32(std::string const &data, int &offset) {
    if (offset + 4 > data.length()) { return absl::nullopt; }   // bounds check before memcpy
    memcpy(&value, data.data() + offset, 4);
    offset += 4;
}
```
- Every `memcpy` is gated by `offset + 4 > data.length()` first; `offset` starts at 0
  and only advances by 4, so the `int`/`size_t` compare cannot be tricked with a
  realistic `data` (reaching a negative-overflowing `offset` needs a ~2 GiB extradata
  blob, which the ffmpeg container layer does not deliver).
- The `count`-driven `parseChannelUpdates` loop bails via `readInt32` the moment data is
  exhausted, so no unbounded index or alloc. Decoded PCM sizing (`480 * _channelCount`,
  `nb_samples * channels`) comes from ffmpeg decode output, not attacker length fields.
- Downstream media bytes flow into ffmpeg (out of scope: heavily fuzzed upstream; the
  relevant residual there is the stale FFmpeg **pin**, tracked separately).

No defect. Confirms the prior passes' assessment of this seam.

## 4. tg_owt / WebRTC version-vs-CVE scoring (the "productive question")

Pinned WebRTC: `build/prepare/prepare.py:1700` checks out
`desktop-app/tg_owt @ 89df288dd6ba5b2ec95b3c5eaf1e7e0c3a870fc4`
(`git submodule update --init --recursive` pulls its libwebrtc base).

- The pinned commit is **recent**: its own HEAD-adjacent history is LLVM 20 / GCC 15
  build-compat churn (e.g. removing `ABSL_ATTRIBUTE_LIFETIME_BOUND` for "LLVM 20 errors
  on void functions whose parameters are applied [[lifetimebound]]"). LLVM 20 dates the
  tree to ≈2025–2026, i.e. it tracks a recent libwebrtc milestone, not a years-stale one.
- Public CVE search for tg_owt / libwebrtc RTP/SRTP heap issues reachable in this window
  returned **no cheap, reachable, known-unfixed match** — the WebRTC CVEs that surface
  (codec / packet parsers) are milestones behind this pin and were fixed upstream before
  this checkout date; nothing maps to an unpatched path reachable through the tdesktop
  glue. This matches the task's own caveat: WebRTC internals are heavily fuzzed by
  OSS-Fuzz and deep bugs here are likely non-novel.
- **Honest conclusion:** no reachable known-CVE "falls out cheaply," so per the task's
  scoping guidance the deep upstream WebRTC surface stays **out of scope**. The
  productive residual in the call path is the **FFmpeg pin** (group-call broadcaster
  media → ffmpeg), tracked as an OPEN item, not tg_owt itself.
- The tgcalls **glue** seams (`EncryptedConnection.cpp`, `Message.cpp`,
  `NetworkManager.cpp`, `SctpDataChannelProviderInterfaceImpl.cpp`, group parsers) were
  traced clean across iterations #1–#5: size-gated intake (`[21, 128 KiB]`), msgKey
  SHA256 verified before parse, string length capped `< 64 KiB`, vector/format counts
  `uint8`, and every hand-rolled parser bounds-checks before `memcpy`/`AppendData`.

## 5. Remaining OPEN items (with fixes)

- **#5 TL recursion stack-overflow (the one PoC'd bug still unfixed).**
  Self-nesting TL boxed types (`RichText` → `textConcat`/`textBold`/… containing
  `RichText`; also `PageBlock`) deserialize with no depth bound
  (`lib_tl/tl/tl_boxed.h` + generated `scheme.cpp`), so a crafted webpage / Instant View
  payload recurses to SIGSEGV. Post-auth server data. PoC:
  `../mtproto-webrtc-deep/poc_richtext_recursion.bin` (~800 KB).
  **Fix:** a parse-depth guard in the generated boxed-read path (`lib_tl` submodule) —
  the correct home is the TL reader, not each call site. This is the single highest-value
  remaining hardening item (working PoC, real crash), but it touches the `lib_tl`
  submodule, so land it there and bump the pin.
- **`ungzip` decompressed-size cap** (`session_private.cpp:2042`). No cap on inflate
  output → zip-bomb memory-DoS (post-auth, malicious DC). The v2 local-storage path
  already caps at 2 MiB; mirror a bound here. Accepted class; cheap defense-in-depth.
- **FFmpeg pin `n6.1.1`** (`build/prepare/prepare.py`) is ~930 commits behind with
  reachable mem-safety fixes missing on the group-call broadcaster media path. **Fix:**
  bump the pin and keep the container/demuxer allowlist. Out-of-tree (build config).

## Bottom line

- Five iterations, four code-fixable bugs across MTProto framing/handshake/crypto —
  **all four now fixed** (MTPROTO-1/2 committed at HEAD; MTPROTO-3, the recursion cap,
  and the RSA-decrypt offset in the working tree, the last landed this pass).
- **No RCE** in MTProto or tgcalls. tgcalls glue traced clean; the vendored WebRTC pin
  is recent enough that **no reachable known-CVE falls out cheaply**, so deep WebRTC
  stays out of scope per the honest threat model.
- **Recommended before release:**
  1. Commit the working-tree fixes (MTPROTO-3, recursion cap, TCP cap, RSA-decrypt) with
     the ASan/standalone regression tests noted in this doc and the prior findings.
  2. Land the **TL parse-depth guard** for #5 in `lib_tl` (only PoC'd crash still open).
  3. Add the `ungzip` output cap and bump the FFmpeg pin (both cheap, both DoS-class).
