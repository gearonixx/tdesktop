# MTProto transport + tgcalls/lib_webrtc — Consolidated Session Findings

Date: 2026-07-11. Branch: `fix/mtproto-preauth-oob` (HEAD `4467e0cbf1`).
Target: Telegram Desktop fork — **MTProto transport / TL deserialization**
(`Telegram/SourceFiles/mtproto/`) and **tgcalls / lib_webrtc** call transport
(`Telegram/ThirdParty/tgcalls/`, `Telegram/lib_webrtc/`) — the network-facing
byte paths that must be memory-safe on malformed / adversarial input before the
next release.

This iteration is a **verification + consolidation pass**: every prior claim was
re-checked against the current source at HEAD, the two memory-safety fixes were
confirmed **committed** (not just working-tree), and the still-open items were
re-confirmed unfixed with exact fix + regression-test recipes. Nothing new was
edited in the source tree this pass — analysis only.

Prior-iteration artifacts this consolidates (kept for their harness setup and
long-form reasoning):
- `../tgcalls-mtproto-audit/SESSION_FINDINGS.md` — iterations #1–#3 (static trace,
  FFmpeg version-diff, `poc_respq_dos`/`poc_notsecure_oob`).
- `../mtproto-tgcalls-review/SESSION_FINDINGS.md` — parallel deep pass + TL fuzzer
  (`fuzz/`), RichText recursion PoC.
- `../mtproto-notsecure-oob/` — first PoC of the not-secure OOB read (F-1/#1).
- `../REVIEW-mtproto-tgcalls-2026-07-11.md`, `../REVIEW-robustness-2026-07-11.md`.

Honest threat model (unchanged, governs all severities below): both areas need a
**malicious/MITM server, a malicious MTProto proxy, or a negotiated/compromised
call** to reach. The pre-auth handshake bugs are the strongest (no key needed).
Everything post-auth needs the session `auth_key`, i.e. a malicious/compromised
DC, not a passive MITM. The realistic ceiling for the residual items is **DoS /
remote crash**, not remote memory corruption or RCE.

---

## TL;DR — status at HEAD `4467e0cbf1`

| # | Defect | File | Class | Reach | Status |
|---|--------|------|-------|-------|--------|
| 1 | `parseNotSecureResponse` byte-len used as mtpPrime element count | `mtproto/connection_abstract.cpp:141` | OOB heap **read** (CWE-125) | **pre-auth** server / MITM / proxy | **FIXED — committed @HEAD**, verified correct |
| 2 | FakeTLS Server Hello short-length subspan | `mtproto/details/mtproto_tls_socket.cpp:757` | OOB read + zero-**write** past `_incoming` | **pre-auth** FakeTLS endpoint | **FIXED — committed @HEAD**, verified correct |
| 3 | RSA `decrypt` leading-zero shift offset (`zeroBytes - res`) | `mtproto/details/mtproto_rsa_public_key.cpp:158` | size_t underflow → abort / OOB **write** | pre-auth config fetch | **OPEN — latent** (dead on OpenSSL3/BoringSSL); 1-line fix |
| 4 | Unbounded recursion in `handleOneReceived` (`msg_container` / `gzip_packed`) | `mtproto/session_private.cpp:1469,1524` | **stack-overflow DoS** | post-auth, malicious/compromised DC | **OPEN**; fix = depth guard |
| 5 | Unbounded recursion in TL deserialization (`RichText` self-nesting; `PageBlock`, etc.) | `lib_tl/tl/tl_boxed.h:39` + generated `scheme.cpp` | **stack-overflow DoS** (SIGSEGV) | post-auth server data (webpage / Instant View) | **OPEN**, fuzzer-found + PoC'd; fix = TL parse-depth guard |
| — | `ungzip` — no decompressed-size cap | `mtproto/session_private.cpp:2025` | zip-bomb memory-**DoS** | post-auth, malicious/compromised DC | **OPEN**; fix = output cap (v2 path already caps @2 MiB) |
| — | FFmpeg pin `n6.1.1` stale (930 behind; 25 reachable mem-safety fixes missing) | `build/prepare/prepare.py:1178` | stale-dep, mostly DoS / some heap-OOB | malicious group-call broadcaster | **OPEN**; fix = bump pin + allowlist `container` |
| — | `tl::vector_type::read` unbounded `count` alloc | `lib_tl/tl/tl_basic_types.h:552` | alloc-DoS (accepted class) | malicious server | **Accepted**; cap on read is the real fix |

**Verified clean this pass** (no defect): `DcKeyBinder::handleResponse`
(`dc_key_binder.cpp:107`) and `BoundKeyCreator::handleResponse`
(`bound_key_creator.cpp:68,83`) — both read server responses via bounds-checked
`error.read(from, from + size)` with no length arithmetic. The two previously
untouched key-binder paths are safe.

Net for release: **the two memory-corruption bugs are landed.** What remains is
one latent (dead-code) correctness bug and three malicious-server-gated DoS
hardening items. None is a shippable RCE; #4/#5/`ungzip` are cheap
defense-in-depth worth taking before release.

---

## Verification of the two committed fixes (#1, #2)

### #1 — `parseNotSecureResponse` OOB read — FIXED @HEAD, correct
`connection_abstract.cpp`. The committed diff replaces the tail with:
```cpp
return gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime));
```
Confirmed against `prepareNotSecurePacket` (`connection_abstract.h:197`), which
writes `*messageLength = (primeCount + padding) << 2` — a **byte** count. `answer`
is `const mtpPrime*` (int32); `make_span`'s second arg is an **element** count.
The old code passed the byte length as an element count → span up to **4×** the
real body; both consumers (`readPQFakeReply` on every connect; the full
`DcKeyCreator` ResPQ/DH exchange) use `answer.size()` as the TL read boundary, so
the deserializer walked past the heap allocation, pre-auth. `answerLen / 4` makes
the span end exactly at `answer + len`; non-multiple-of-4 lengths floor down
(read fewer, never more). Reachable by malicious DC / MITM / MTProto proxy. Fix
is complete and correct. (Prior PoC: `../mtproto-notsecure-oob/`,
`../tgcalls-mtproto-audit/poc_respq_dos.cpp`.)

### #2 — FakeTLS Server Hello short-length — FIXED @HEAD, correct
`mtproto_tls_socket.cpp`. `_serverHelloLength` is assembled from two
attacker-controlled 16-bit part-lengths and can collapse to ~16, while
`checkHelloDigest` takes a **fixed** digest subspan at offset [43,75) of
`_incoming`. On a too-short hello the subspan (read + zero-write) runs past the
detached `_incoming` buffer — an OOB read plus OOB zero-**write** (the more
serious of the two original bugs). The committed guard rejects short hellos before
the subspan:
```cpp
if (_serverHelloLength < kServerHelloDigestPosition + kHelloDigestLength) { … return; }
```
Confirmed this is the only hello subspan not already covered by
`requiredHelloPartReady()`. Fix is complete and correct.

---

## OPEN #4 — unbounded recursion in `handleOneReceived` → stack-overflow DoS

`session_private.cpp:1454`. **Re-confirmed unfixed at HEAD.** `handleOneReceived`
recurses into itself for two nesting constructs with **no depth counter**:
- `mtpc_gzip_packed` (`:1469`): `return handleOneReceived(response.data(), …)` on
  the decompressed buffer. The local `mtpBuffer response` has a destructor that
  runs after the call, so it is **not** a true tail call — the frame (and each
  decompressed buffer) stays live down the chain.
- `mtpc_msg_container` (`:1524`): `res = handleOneReceived(from, otherEnd, …)` for
  each sub-message; **non-tail** (the loop inspects `res` after). Each nesting
  level adds a real frame.

`OuterInfo` (`session_private.h:122`) carries `outerMsgId / serverSalt /
serverTime / badTime` — **no depth field** — and no depth is threaded through the
signature. The per-level width checks (`from+4 >= end`, `bytes.v & 0x03 ||
bytes.v < 4`, `otherEnd > end`) bound each level in-buffer but never the depth.

**Quantified.** Each container-in-container level costs ~6 primes / 24 B
(`type(1) + count(1) + submsg header(4)`). `kMaxMessageLength = 16 MiB` (`:53`)
caps the outer message → max depth ~700 k, but an ~8 MiB thread stack exhausts at
**~30–40 k** frames (~100–300 B each) — a **~1 MiB** crafted container. Result:
stack overflow → SIGSEGV. On a guard-page platform (Linux/macOS/Windows) this is
a **clean crash = DoS**, not corruption.

**Reach.** Post-auth only: the message must decrypt under the session `auth_key`
and pass the `msg_key` SHA-256 check (`:1331`), so it needs a
**malicious/compromised DC**, not a MITM. `registerMsgId` (`:1520`) returns
`Success` for any fresh increasing `msgId` — attacker-controlled inside the
crafted container — so nothing throttles the descent. Same code ships in upstream
tdesktop → frame as an upstream robustness gap we also carry.

**Fix (owned source, low-risk).** Add a depth to `OuterInfo` (or a parameter) and
reject past a small bound. Legit MTProto nests shallowly (a container of
messages, optionally one `gzip_packed` body), so ~8 is ample:
```cpp
// session_private.h, struct OuterInfo:
int depth = 0;
// session_private.cpp, top of handleOneReceived:
if (info.depth > kMaxHandleDepth /* e.g. 8 */) return HandleResult::ParseError;
// at each recursion site pass { …, .depth = info.depth + 1 }
```

**Regression test (no network).** Build a decrypted buffer = N-deep chain of
single-message `msg_container`s (fresh increasing msg_ids, each `bytes.v`
covering the inner remainder) and call `handleOneReceived` directly: pre-fix
N≈50 000 overflows the stack (ASan: stack-overflow), post-fix it returns
`ParseError` at the depth limit.

---

## OPEN #5 — unbounded recursion in TL deserialization (RichText, PageBlock, …)

`lib_tl/tl/tl_boxed.h:39` + generated `scheme.cpp`. **Re-confirmed: no depth
guard anywhere in `lib_tl`** (`grep -rn depth lib_tl/tl` is empty). `boxed::read`
reads a constructor id and calls `bare::read`, which for a self-referential type
recurses through the boxed reader again — unbounded.

The recursive TL types are real and numerous (`scheme/api.tl:906-915`):
```
textBold#6724abc4     text:RichText  = RichText;
textItalic#d912a59c   text:RichText  = RichText;
textUnderline#c12622c4 text:RichText = RichText;
textStrike#9bf8bb95   text:RichText  = RichText;
textFixed#6c3f19b9    text:RichText  = RichText;
textUrl#3c2884c1      text:RichText url:string webpage_id:long = RichText;
textEmail#de5a0dd6    text:RichText email:string = RichText;
textConcat#7e6260d7   texts:Vector<RichText> = RichText;
```
A `textBold(textBold(textBold(…)))` chain (each level ~2 primes / 8 B) recurses in
the generated `MTPRichText::read` → stack overflow. `PageBlock`, nested
`MessageEntity`/`PageListItem` types have the same shape.

**Reach.** Post-auth, server-delivered content: `RichText` arrives via
`messages.getWebPage` / Instant View pages and is consumed at
`SourceFiles/iv/iv_rich_page.cpp` and `history/view/history_view_message.cpp`. So
the trigger is a webpage-preview / IV page the server chooses to deliver —
malicious/compromised server (or a server relaying attacker-crafted IV content).
Slightly wider surface than #4 because IV content is the ordinary
"server-renders-a-webpage" path.

**Impact.** Stack overflow → SIGSEGV → clean DoS on opening / previewing the
message. Not corruption/RCE.

**PoC (prior).** The TL fuzzer (`../mtproto-tgcalls-review/fuzz/`) mutator
discovered `textBold` self-nesting; scaled up it reproduces the overflow
(`findings/richtext_recursion/`). Eyeballing the individually-bounded primitives
would not have surfaced it — this is the fuzzer's payoff.

**Fix (needs care — touches generated read path).** Add a parse-depth counter to
the `lib_tl` reader used by generated `read()` and reject beyond a bound
(e.g. 64). This is the single systemic fix for the whole class (RichText,
PageBlock, …), unlike per-type patches. Because it touches the `lib_tl` submodule
and codegen contract, land it there deliberately (not a blind edit). Alternative
short-term: cap RichText nesting at the IV consumer. **Regression test:** a boxed
`MTPRichText` buffer with N=100 000 nested `textBold` → pre-fix stack-overflow,
post-fix rejected at the depth bound.

---

## OPEN latent #3 — RSA `decrypt` leading-zero compensation offset

`mtproto_rsa_public_key.cpp:156-159`, `RSAPublicKey::Private::decrypt`. When
`RSA_public_decrypt(…, RSA_NO_PADDING)` returns `res < kDecryptSize (256)`, the
left-align uses `zeroBytes - res` (= `256 - 2*res`) as the destination offset:
```cpp
} else if (auto zeroBytes = kDecryptSize - res) {     // res in [0,255]
    bytes::move(resultBytes.subspan(zeroBytes - res, res), resultBytes.subspan(0, res));
    bytes::set_with_const(resultBytes.subspan(0, zeroBytes - res), gsl::byte{});
```
`zeroBytes - res` goes **negative for res > 128**; `subspan`'s `size_t` offset
wraps huge → gsl `Expects` abort (DoS) or, with contracts compiled out, a wild
`bytes::move` (heap OOB write). The sibling `encrypt()` (`:140`) is correct:
`subspan(zeroBytes, res)`.

**Reach: effectively dead code.** On OpenSSL 1.1+/3.x and BoringSSL,
`RSA_public_decrypt(RSA_NO_PADDING)` returns the **full** modulus size (256,
left-zero-padded by `RSA_padding_check_none`), so `res == 256`, `zeroBytes == 0`,
and the buggy branch is never entered. It only bites a backend that strips leading
zeros. Caller: `special_config_request.cpp:448` `decryptSimpleConfig` (censorship
-circumvention config fetch), pre-auth but only if that fallback is tampered.

**Fix (1 line, defense-in-depth):** mirror `encrypt` → `subspan(zeroBytes, res)`
and `subspan(0, zeroBytes)`, or `Assert(res == kDecryptSize)`. Not shippable on
current backends, but a real correctness trap that should not be left.

---

## OPEN residual DoS — `ungzip` has no decompressed-size cap

`session_private.cpp:2025`. **Re-confirmed unfixed.** The inflate loop
`while (!stream.avail_out) { result.resize(result.size() + unpackedChunk); … }`
grows `result` until the stream ends, with **no output cap** → a zip-bomb
`gzip_packed` body (reachable at `:1465` and `:1851`) drives unbounded memory
allocation → memory-DoS. Post-auth, malicious/compromised DC.

Note the newer tgcalls **v2 signaling path already caps gunzip at 2 MiB**
(`InstanceV2Impl.cpp:1681`). **Fix:** mirror that — cap the decompressed size to a
few MiB and bail on overflow. Cheap parity hardening.

---

## OPEN stale-dep — FFmpeg pin `n6.1.1` (reachable group-call demuxers)

`build/prepare/prepare.py:1178` pins `FFmpeg n6.1.1` (Nov 2023). The group-call
video path reads an **attacker-controlled container name** from the wire
(`VideoStreamingPart.cpp:238` → `av_find_input_format(container.c_str())` →
`avformat_open_input` over attacker bytes), so a malicious broadcaster picks which
demuxer parses their bytes. Prior pass enumerated the full delta:
`n6.1.1 … release/6.1` = **930 commits behind**, of which **25 memory-safety
fixes land in enabled+reachable modules** (mov ×7, matroskadec ×5, ogg ×3, wav,
plus vp9/vp8dsp/flac/h264/vorbis/av1 decoder fixes). Mostly DoS, some heap-OOB;
semi-0-click once in a group call. Not novel bugs — a **stale-dependency
reachability** finding. **Fix:** bump the pin to current `release/6.1` (or 7.x)
and, defensively, allowlist the `container` string instead of passing it raw.

---

## Coverage swept clean this pass (evidence)

- **`DcKeyBinder::handleResponse`** (`dc_key_binder.cpp:107`): asserts non-empty,
  reads `response[0]`, then either `boolTrue` or bounds-checked `error.read(from,
  end)`. No length arithmetic on server bytes. Clean.
- **`BoundKeyCreator::handleResponse`** (`bound_key_creator.cpp:68,83`): delegates
  to `_binder->handleResponse`; the `error.read(from, from + buffer.size())` is
  bounds-checked. Clean.
- **Committed fixes #1/#2**: re-derived from source; both correct and complete.
- **tgcalls pin** `616810f` (2026-04-15) — recent; WebRTC/tg_owt tracks upstream
  master (0 behind, prior pass). The FFmpeg pin is the lone stale outlier.

Everything else in the two prior SESSION_FINDINGS remains valid — TCP/HTTP
framing caps, encrypted-envelope checks, TL primitive bounds, EncryptedConnection
/ Message.cpp bounds, group audio/video hand-rolled readers, and the DH
key-agreement math (validated `p`/`g`, `IsGoodModExpFirst` on `g_a`/`g_b`, the
`g_a_hash` commitment, key-fingerprint compare) — all traced and clean.

---

## NEXT STEPS (highest value first)

1. **Land #4** (`handleOneReceived` depth guard) + regression test — owned source,
   small, clearly correct, closes a ~1 MiB remote-crash DoS.
2. **Land the `ungzip` output cap** — parity with the v2 2 MiB cap; one check.
3. **Land #5** (TL parse-depth guard in `lib_tl`) — systemic fix for the whole
   RichText/PageBlock recursion class; land in the submodule deliberately.
4. **Fix #3 defensively** (`subspan(zeroBytes, res)` or `Assert(res == 256)`).
5. **Bump FFmpeg** off `n6.1.1` and allowlist the group-call `container` name.

## Key file:line references (verified this session, HEAD `4467e0cbf1`)

- `connection_abstract.cpp:141` — `parseNotSecureResponse` (#1 fix, committed).
- `connection_abstract.h:197` — `prepareNotSecurePacket` (`messageLength << 2`, bytes).
- `details/mtproto_tls_socket.cpp:757` — `checkHelloDigest` (#2 fix, committed).
- `details/mtproto_rsa_public_key.cpp:156-159` — `decrypt` (#3) vs `encrypt` `:140`.
- `session_private.cpp:1454-1535` — `handleOneReceived` (#4: recursion @1469, @1524).
- `session_private.cpp:122` (`.h`) — `struct OuterInfo` (no depth field).
- `session_private.cpp:2025` — `ungzip` (no output cap).
- `lib_tl/tl/tl_boxed.h:31-40` — `boxed::read` (no depth; #5 systemic site).
- `SourceFiles/mtproto/scheme/api.tl:906-915` — recursive `RichText` constructors.
- `SourceFiles/iv/iv_rich_page.cpp`, `history/view/history_view_message.cpp` — RichText consumers (#5 reach).
- `details/mtproto_dc_key_binder.cpp:107`, `mtproto_bound_key_creator.cpp:68,83` — clean.
- `build/prepare/prepare.py:1178` — FFmpeg `n6.1.1` pin (stale).
