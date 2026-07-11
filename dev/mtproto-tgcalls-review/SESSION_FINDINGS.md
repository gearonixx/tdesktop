# MTProto transport + tgcalls/lib_webrtc — Session Findings (defensive robustness review)

Date: 2026-07-11. Branch: `test/collapsed-chats-alert-bar`. Target: Telegram Desktop,
modules **MTProto transport / TL deserialization** (`Telegram/SourceFiles/mtproto/`) and
**tgcalls / lib_webrtc** call transport (`Telegram/ThirdParty/tgcalls/`,
`Telegram/lib_webrtc/`) — the network-facing byte paths that must be memory-safe on
malformed / adversarial input before the next release.

Method this session: **static tracing**, not fuzzing. For these two areas a fuzzer needs
the full data model (auth keys, session state, crypto handshake) to reach the interesting
handlers, so tracing the read path end-to-end (bytes → framing gate → decrypt → TL/parse
handler) beats standing up a harness. tgcalls was **version-scored against known CVEs
first** (cheapest signal), then only its glue seams were read. Reachability was confirmed
for every claim before assigning impact.

Companion long-form report: `../REVIEW-mtproto-tgcalls-2026-07-11.md`.
Prior related artifact (finding #1 PoC): `../mtproto-notsecure-oob/` (FINDING.md + ASan repro).

---

## TL;DR — iteration #5 (apply the safe hardening for the reachable open findings)

Date: 2026-07-11 (later pass). Branch: `fix/mtproto-preauth-oob`. Iteration #4 confirmed the
open findings and left the fixes as recommendations. This pass **applies the two lowest-risk,
highest-value checks** — the ones that close a reachable DoS with zero regression risk to
legitimate traffic — with a self-validating regression PoC. Fixes are in the working tree
(uncommitted), source diff below.

**Applied fix A — O-1: clamp `Version0::readPacketLength` to `kPacketSizeMax`.**
`connection_tcp.cpp:118`. The abridged-transport `0x7F` extended-length branch returned
`int(ints << 2) + 4` (up to exactly 64 MiB) with no upper bound, while the sibling `VersionD`
path already clamps with `value < kPacketSizeMax` (`:213`). A MITM/malicious proxy could drive
a single ~64 MiB buffer allocation. Now mirrors VersionD:
```cpp
if (ints < 0x7F) { return kInvalidSize; }
const auto result = int(ints << 2) + 4;
return (result < kPacketSizeMax) ? result : kInvalidSize;
```
Zero regression risk: legitimate abridged packets are far below 64 MiB, and the strict `<`
matches the existing VersionD semantics.

**Applied fix B — #4: depth guard on `handleOneReceived` recursion.**
`session_private.cpp` + `session_private.h`. Added `int depth = 0;` to the by-value `OuterInfo`
and a guard at the top of `handleOneReceived` (after `Expects(from < end)`):
```cpp
if (info.depth >= kMaxHandleDepth /* = 16 */) {
    LOG(("Message Error: too deeply nested container/gzip_packed received."));
    return HandleResult::ParseError;
}
++info.depth;
```
Because `info` is passed by value, the counter increments independently down each recursion
branch and is unaffected by sibling sub-messages in a container loop. Legit MTProto nests
shallowly (container → optional gzip_packed body), so `kMaxHandleDepth = 16` is far above real
traffic. Closes the stack-overflow DoS (malicious/compromised DC).

**Regression PoC (built + run, self-validating):** `findings/recursion_guard/`
(`poc_container_depth.cpp` + `repro.sh` + `FINDING.md`). A faithful headless model of the
container recursion (same wire encoding, same guards, `OuterInfo`-style by-value depth):
- **guarded** (models the fix): rejects at depth 16, `maxDepthSeen=16`, `PASS`, exit 0.
- **unguarded** (models old code) under a 512 KiB stack (to match the real ~100–300 B frame on
  an 8 MiB stack): `AddressSanitizer: stack-overflow ... in handleOneReceived`, nonzero exit.
  `repro.sh` asserts guarded-passes ∧ unguarded-crashes → prints `OK`.

**Source diff (this pass):**
```
 connection_tcp.cpp  |  6 +++++-   # O-1 clamp
 session_private.cpp | 17 +++++++++++++++++  # kMaxHandleDepth + guard
 session_private.h    |  1 +   # OuterInfo::depth
```

**Deliberately NOT applied this pass (and why):**
- **`ungzip` output cap (gz):** correct in principle, but the safe ceiling is not obviously
  2 MiB (the v2 signaling value) — the main session legitimately decompresses large update
  batches, so an under-sized cap would *drop real updates* (functional regression). Needs the
  real legit-max before landing; recommend a generous cap (≥ a few × `kMaxMessageLength`), not
  a copy of the v2 number. Left as a documented recommendation rather than a risky guess.
- **#5 RichText TL parse-depth guard:** the right fix lives in the generated boxed-read
  wrapper / a thread-local depth counter, which touches codegen infrastructure — larger blast
  radius than the two checks above. Documented in iteration #4; still open.
- **#3 RSA offset:** dead code on OpenSSL3/BoringSSL; 1-line defensive fix deferred (no
  reachable impact on our backends).

**Net after this pass:** of the five open findings from iteration #4, the two **reachable**
DoS items with a clean, safe fix (O-1, #4) are now **fixed + regression-tested**. Remaining
open: #5 (TL recursion, codegen change), gz (`ungzip` cap, needs legit-max), #3 (dead code),
and F-1 (stale FFmpeg — dependency bump). All remaining items are DoS-class or dead code,
gated behind a malicious/compromised DC or MITM/proxy — unchanged from the honest threat model.

---

## TL;DR — iteration #4 (consolidation + binder-path close-out + open-item verification)

Date: 2026-07-11 (later pass). Branch: `fix/mtproto-preauth-oob`. This iteration
**consumes and reconciles the two prior sessions** (this doc + `../tgcalls-mtproto-audit/
SESSION_FINDINGS.md`), verifies the current source state, closes the last untraced
MTProto seam, and re-confirms the still-open findings against the live tree. No new
memory-corruption bug surfaced; the value here is *state truth* before release.

**State change since the prior sessions:** bugs **#1 and #2 are now committed**
(`4467e0cbf1` "mtproto: fix two pre-auth OOB accesses in handshake parsing") and I
re-verified both fixes are present and correct in the working tree:
- `connection_abstract.cpp:149` — `gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime))`
  (byte-length → prime-count), with the explanatory comment at `:141-148`.
- `mtproto_tls_socket.cpp:763` — `if (_serverHelloLength < kServerHelloDigestPosition +
  kHelloDigestLength) { logError(888, ...); handleError(); return; }` guarding the digest
  subspan at `:768-773`. This is the one hello subspan not already covered by
  `requiredHelloPartReady()`; the guard is minimal and correct.

**New coverage this pass — the last untraced MTProto read seam, and it is clean:**
the temp-key **bind** response parse (`mtproto_dc_key_binder.cpp` /
`mtproto_bound_key_creator.cpp`), which both prior sessions listed under "not yet
examined." All three read paths are bounds-safe:
- `DcKeyBinder::handleResponse` (`dc_key_binder.cpp:107-126`): the raw index `response[0]`
  (`:113`) is guarded by `Expects(!response.isEmpty())` (`:108`); `error.read(from, end)`
  (`:115`) uses the correct `end = from + response.size()`. No OOB.
- `BoundKeyCreator::handleBindResponse` (`bound_key_creator.cpp:67-72`) is a thin forwarder
  to the above.
- `IsDestroyedTemporaryKeyError` (`bound_key_creator.cpp:80-90`): no raw index at all —
  `error.read(from, from + buffer.size())` on a possibly-empty buffer just returns `false`
  on empty (the TL reader is `HasBytes`-guarded). Safe.

With the binder path cleared, **every hand-written MTProto read path named in the file map
has now been traced** (framing, decrypt/envelope, container/rpc_result, DH key creation,
key binding, DoH/special-config, HTTP). The only defects across all of it are the five in
the table below.

**Still-open findings re-verified against the live tree (all confirmed still present):**

| # | Defect | Live-tree evidence | Class / threat model | Fix status |
|---|--------|--------------------|----------------------|------------|
| 4 | Unbounded recursion in `handleOneReceived` (nested `msg_container` / `gzip_packed`) | `session_private.cpp:1469,1524` — recursive calls; **no** `depth` field in `OuterInfo` (`session_private.h:122-127`), grep for `depth`/`kMaxHandleDepth` = none | stack-overflow → clean SIGSEGV (**DoS**), post-auth, malicious/compromised **DC** | **Open** — fix below |
| 5 | Unbounded recursion in TL `RichText` deserialization | `out/Telegram/gen/scheme.cpp:61741` — a RichText ctor field recurses via `_text.read(...)` into `MTPRichText`; 20 self-nesting RichText ctors (`textBold`/`textItalic`/`textUrl`/`textConcat`/…) | stack-overflow → SIGSEGV (**DoS**), post-auth server data (webpage / Instant View) | **Open** — TL parse-depth guard |
| 3 | RSA `decrypt` leading-zero offset math (`zeroBytes - res` underflow) | `mtproto_rsa_public_key.cpp:156-159` | assert-abort / OOB write, **but dead on OpenSSL3/BoringSSL** (`RSA_padding_check_none` returns full `num`) | **Open (latent)** — 1-line, defense-in-depth |
| O-1 | `Version0::readPacketLength` `0x7F` branch lacks the `kPacketSizeMax` clamp `VersionD` has | `connection_tcp.cpp` `Version0` path | ~64 MB alloc-DoS, MITM/proxy | **Open** — 1-line clamp |
| gz | `ungzip` has no decompressed-size cap | `session_private.cpp:2051-2054` — `while (!stream.avail_out) result.resize(+unpackedChunk)`, no ceiling; contrast v2 path caps gunzip at 2 MiB (`InstanceV2Impl.cpp:1681`) | zip-bomb memory-DoS, post-auth malicious DC | **Open** — mirror the v2 cap |

**Precise fixes for the two headline open items (recommended, not yet applied — protocol
core, so flagged for review rather than silently changed):**

- **#4 depth guard.** `OuterInfo` is passed *by value* into `handleOneReceived`, so a depth
  counter threads for free — each recursion gets its own incremented copy. Add `int depth = 0;`
  to `OuterInfo` (`session_private.h:122`) and, at the top of `handleOneReceived` (after the
  `Expects(from < end)`), reject over-nesting before dispatch:
  ```cpp
  if (info.depth > kMaxHandleDepth /* e.g. 8 */) {
      return HandleResult::ParseError;
  }
  ++info.depth; // copy is local; siblings in the container loop are unaffected
  ```
  Legit MTProto nests shallowly (top-level container of messages, optional `gzip_packed`
  body), so a bound of 4–8 is safe. Max reachable depth today ≈ `kMaxMessageLength (16 MiB)`
  / 24 B per container level ≈ 700 k; an 8 MiB thread stack dies at ~30–40 k levels
  (~1 MiB crafted message). Note `gzip_packed` recursion also pins each decompressed buffer
  down the chain, so a depth guard bounds memory as well as stack.
- **#5 TL parse-depth guard.** The clean place is the generated boxed-read wrapper
  (`lib_tl/tl/tl_boxed.h`) or a thread-local depth counter incremented in `MTPRichText::read`;
  reject beyond a small bound. Fuzzer-found and PoC'd in the prior session
  (`findings/richtext_recursion/`).

**Honest overall verdict (unchanged and now well-supported):** the two genuinely
network-reachable memory-safety bugs (#1 OOB read, #2 OOB read + zero-write) were **pre-auth**
and are **fixed and committed**. Everything still open is either **DoS-class** (stack overflow
or alloc/zip-bomb) gated behind a **malicious/compromised DC or MITM/proxy**, or **dead code**
on the crypto backends we actually link (#3). That is exactly the residual-risk shape the
brief predicted for this surface — "post-deserialization interpretation, not raw memory
corruption." tgcalls/lib_webrtc added no new memory-corruption finding; its one real exposure
is the **stale pinned FFmpeg** (F-1) reached via the attacker-chosen `container` in the
group-call video path, a stale-dependency reachability issue, not a glue bug.

**Recommended before release (priority order):** (1) land depth guards for #4 and #5 with the
ASan regression tests sketched below — cheapest closure of the two open *reachable* DoS bugs;
(2) cap `ungzip` to parity with the v2 path; (3) clamp O-1; (4) fix #3 defensively; (5) bump
FFmpeg and allowlist `container` (F-1). Regression tests for #1/#2 (the committed fixes) are
still worth adding: a crafted not-secure response with oversized byte-length + large inner
vector count, and a Server Hello with `part2Size=part4Size=0` (`_serverHelloLength=16`).

---

## TL;DR — what was found

Three real code defects. **Two are memory-safety bugs, both pre-auth, both already fixed
in the working tree** (uncommitted) and re-confirmed correct this session. One is a new
latent bug that is dead code on the crypto backends we actually link. No *new*
reachable memory-corruption defect surfaced in the fresh sweep. The remaining risk is the
previously-accepted alloc / decompress-DoS class, reachable only from a malicious/MITM
server.

| # | Defect | File | Class | Reachability | Status |
|---|--------|------|-------|--------------|--------|
| 1 | `parseNotSecureResponse` byte-len used as element count | `mtproto/connection_abstract.cpp:141` | OOB heap **read** (CWE-125) | **pre-auth**, malicious/MITM server or MTProto proxy | **Fixed** (working tree), PoC'd |
| 2 | FakeTLS Server Hello short-length | `mtproto/details/mtproto_tls_socket.cpp:757` | OOB read + zero-**write** past `_incoming` / DoS | **pre-auth**, malicious FakeTLS endpoint | **Fixed** (working tree) |
| 3 | RSA `decrypt` leading-zero shift offset math | `mtproto/details/mtproto_rsa_public_key.cpp:158` | size_t underflow → assert-abort / OOB **write** | pre-auth config fetch, **but dead on OpenSSL3/BoringSSL** | Open (latent), fix = 1 line |
| 4 | Unbounded recursion in `handleOneReceived` (nested `msg_container` / `gzip_packed`) | `mtproto/session_private.cpp:1472,1524` | **stack-overflow DoS** (clean crash via guard page) | post-auth, **malicious/compromised DC** (needs session key; not MITM) | **Open (new)**, fix = recursion-depth guard |
| 5 | Unbounded recursion in TL `RichText` deserialization (`textBold`→`RichText`→…) | `gen/scheme.cpp:50645,51061` + `tl_boxed.h:39` | **stack-overflow DoS** (SIGSEGV) | post-auth server data (webpage/Instant View); malicious/compromised DC | **Open (new)**, **fuzzer-found + PoC'd**, fix = TL parse-depth guard |

Residual DoS (known class, malicious-server-gated, NOT corruption):
- `SessionPrivate::ungzip` (`session_private.cpp:2025`) has **no decompressed-size cap** →
  zip-bomb memory-DoS. Note: the newer tgcalls **v2 signaling path already caps gunzip at
  2 MiB** (`InstanceV2Impl.cpp:1681`) — mirror that cap here (cheap hardening win).
- `tl::vector_type::read` allocates `QVector<T>(count, T())` from an unbounded attacker
  `count` before reading elements (`tl_basic_types.h:552`) — the previously-cleared
  alloc-DoS.

---

## NEW finding #4 — unbounded recursion in `handleOneReceived` → stack-overflow DoS

`Telegram/SourceFiles/mtproto/session_private.cpp:1454` (`handleOneReceived`), recursion
sites `:1469` (`gzip_packed`) and `:1524` (`msg_container`). Found by *thinking harder about
the dispatch layer* — the crypto audit did not cover it because it sits **above** the crypto,
in message interpretation, after the `msg_key` check passes.

**Root cause — no recursion-depth bound.** `handleOneReceived` dispatches a decrypted message
and recurses into itself for two nesting constructs, with no depth counter anywhere (the
signature carries `from`, `end`, `msgId`, `OuterInfo` — none is a depth, and `OuterInfo` has
no depth field):
- `mtpc_msg_container` (`:1472`): for each sub-message it calls
  `handleOneReceived(from, otherEnd, inMsgId.v, info)` at `:1524`. **Non-tail** call (the loop
  continues and inspects `res` after), so every nesting level adds a real stack frame. A
  container whose single sub-message is itself a container recurses one level deeper.
- `mtpc_gzip_packed` (`:1463`): `return handleOneReceived(ungzip(...)...)` at `:1469`. The
  local `mtpBuffer response` has a destructor that runs *after* the call returns, so this is
  not a true tail call either — it also grows the stack (and pins each decompressed buffer
  down the chain).

**Why the existing bounds don't stop it.** The per-level checks (`from+4 >= end`,
`bytes.v & 0x03 || bytes.v < 4`, `otherEnd > end`) bound the *width* and keep each level
in-buffer, but not the *depth*. Each nesting level costs only ~6 primes (24 bytes):
`type(1) + count(1) + submsg header(4)`. `registerMsgId` (`:1520`) returns `Success` for any
fresh, increasing `msgId` — which the attacker fully controls inside the crafted container —
and `shrink()` runs only *after* `handleOneReceived` returns (`:1414`), so nothing throttles
the descent.

**Reachability & impact.** `kMaxMessageLength = 16 MiB` (`:53`) caps the outer message, so max
nesting depth is ~16 MiB / 24 B ≈ **700 000**. A `handleOneReceived` frame is ~100–300 B, so
an 8 MiB thread stack is exhausted at **~30–40 k levels** — a **~1 MiB** crafted container
message. Result: stack overflow → SIGSEGV. On any platform with a stack guard page
(Linux/macOS/Windows) this is a **clean crash = DoS**, not memory corruption or RCE.

Reachable **only post-authentication**: the message must be encrypted with the session
`auth_key` and pass the `msg_key` SHA-256 check (`:1331`), so this needs a **malicious or
compromised DC**, not a network MITM (who lacks `auth_key`) and not an anonymous peer — the
weaker threat model this review scopes to; treat as defense-in-depth against a hostile server.
The same code exists in upstream tdesktop, so frame it as an **upstream robustness gap we also
ship**, not a fork regression.

**Fix (minimal).** Thread a depth counter through `handleOneReceived` (or add it to
`OuterInfo`) and reject beyond a small bound. Legitimate MTProto nests shallowly (a top-level
container of messages, optionally a `gzip_packed` body), so a limit of ~4–8 is safe:

```cpp
if (++info.depth > kMaxHandleDepth /* e.g. 8 */) {
    return HandleResult::ParseError; // reject over-nested message
}
```

**Regression test.** Build a decrypted buffer = N-deep chain of single-message
`msg_container`s (fresh increasing msg_ids, each `bytes.v` covering the inner remainder);
pre-fix N≈50 000 overflows the stack (ASan: stack-overflow), post-fix it is rejected at the
depth limit. No network needed — feed the buffer straight to `handleOneReceived`.

---

## CONFIRMED memory-safety bug #1 — pre-auth OOB read in `parseNotSecureResponse`

`Telegram/SourceFiles/mtproto/connection_abstract.cpp:141`. CWE-125, pre-authentication.
Introduced upstream (commit `941288b58e`, 2018) — also present in official Telegram Desktop.

**Root cause — units mismatch (bytes vs. mtpPrime elements).** The message length field
`answer[4]` is validated as a **byte** count (matches the send side
`prepareNotSecurePacket` writing `*messageLength = (...) << 2`), but the return passed it to
`gsl::make_span(answer + 5, answerLen)` where `answer` is `mtpPrime*` and the second arg is
an **element** count. A byte length used as a prime count → the returned span, and the
`end` pointer both callers derive from it via `result.read(from, from + answer.size())`,
extends up to **4× past** the real packet body / heap allocation.

**Consumers (both pre-auth handshake paths):**
- `AbstractConnection::readPQFakeReply` — fake `req_pq` probe sent on **every** TCP/HTTP
  connect (`connection_tcp.cpp:602`, `connection_http.cpp:210`).
- `DcKeyCreator::readNotSecureResponse<T>` — the full DH auth-key creation exchange
  (`mtproto_dc_key_creator.cpp:435,455`).

With the inflated `end`, every TL bounds check inside `read()` runs against the wrong
boundary, so an attacker-controlled inner TL length (`bytes`/`string` prefix or `Vector`
count) walks the deserializer past the allocation.

**Impact.** OOB heap read, size/distance attacker-tunable. Realistic: remotely triggerable
crash (SIGSEGV) at connection setup → DoS. Info-leak potential low (over-read lands in
`resPQ.pq`/fingerprints, used only for local checks, not echoed to the wire). No write
primitive.

**Fix (in working tree, verified correct):**
```cpp
return gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime));
```
Legit traffic unchanged (`answerLen` is always `4*(len-5)`, so `answerLen/4 == len-5`, the
full body); sub-prime malformed lengths truncate harmlessly.

**Repro (exists):** `../mtproto-notsecure-oob/repro.cpp` models the function + TL string reader.
```
c++ -std=c++17 -fsanitize=address -g repro.cpp -o repro
./repro buggy   # ASan: heap-buffer-overflow READ, 0 bytes after a 420-byte region
./repro fixed   # span trimmed to real body; reader rejects; no OOB
```

---

## CONFIRMED memory-safety bug #2 — FakeTLS Server Hello short-length

`Telegram/SourceFiles/mtproto/details/mtproto_tls_socket.cpp:757` (`checkHelloDigest`).
Pre-auth, FakeTLS transport, malicious/MITM endpoint.

**Root cause.** The Server Hello part2/part4 length fields are fully attacker-controlled, so
`_serverHelloLength` can be smaller than the fixed digest region. The code then did
`bytes::make_detached_span(_incoming).subspan(0, kHelloDigestLength + _serverHelloLength)`
and zero-wrote the digest slot at a fixed offset — both can reach past `_incoming` when the
hello is too short. OOB read plus an OOB zero-**write** on the detached span → corruption /
crash.

**Exact outcome pinned this session.** With part2/part4 = 0, `_serverHelloLength` collapses
to ~16 and `_incoming` can be exactly 48 bytes, so `digest = fulldata.subspan(32 + 11, 32)`
= `subspan(43, 32)` on a 48-byte span. GSL `make_subspan` (`ThirdParty/GSL/include/gsl/span:760`)
runs `Expects(size() - offset >= count)` = `Expects(5 >= 32)`. A repo-wide grep confirms **no**
`GSL_THROW_ON_CONTRACT_VIOLATION` / `GSL_UNENFORCED_ON_CONTRACT_VIOLATION` is defined by us,
so `Expects` resolves to the default `std::terminate()` — pre-fix this is a **deterministic
unauthenticated remote crash (DoS)**, aborting *before* the digest zero-write executes. The
OOB zero-**write** of up to 27 bytes past `_incoming` only materializes in a build that
compiles GSL contracts out. Either way the `>= 43` guard is the correct and minimal fix.

**Fix (in working tree, verified correct):** reject short hellos up front —
```cpp
if (_serverHelloLength < kServerHelloDigestPosition + kHelloDigestLength) {
    logError(888, "Bad Server Hello length."); handleError(); return;
}
```

---

## NEW latent bug #3 — RSA `decrypt` leading-zero compensation offset

`Telegram/SourceFiles/mtproto/details/mtproto_rsa_public_key.cpp:156-159`,
`RSAPublicKey::Private::decrypt`.

**Root cause — asymmetric with `encrypt`.** When `RSA_public_decrypt(..., RSA_NO_PADDING)`
returns fewer than `kDecryptSize` (256) bytes, the code left-shifts the recovered bytes to
right-align them. It uses `zeroBytes - res` for the destination offset:
```cpp
} else if (auto zeroBytes = kDecryptSize - res) {              // res in [0,255]
    auto resultBytes = gsl::make_span(result);
    bytes::move(resultBytes.subspan(zeroBytes - res, res), resultBytes.subspan(0, res));
    bytes::set_with_const(resultBytes.subspan(0, zeroBytes - res), gsl::byte{});
```
The sibling `encrypt()` (`:140`) is correct: `subspan(zeroBytes, res)`.
`zeroBytes - res == 256 - 2*res` goes **negative for res > 128** (e.g. res=255 → −254).
`subspan`'s offset is `size_t`, so a negative int becomes an enormous offset →
gsl `Expects(offset <= size())` aborts (DoS), or, with contracts disabled, a wild
`bytes::move` (heap OOB write). For res in [1,128] it silently writes to the wrong offset →
garbage plaintext → later digest mismatch (functional only).

**Caller (pre-auth):** `special_config_request.cpp:448` `decryptSimpleConfig` — the
censorship-circumvention config fetch (DoH TXT / Firebase / Firestore). The 256-byte RSA
input is attacker-influenceable if that fallback response is tampered.

**Reachability: effectively unreachable today (dead code).** On OpenSSL 1.1+/3.x and
BoringSSL, `RSA_public_decrypt(RSA_NO_PADDING)` returns the **full** modulus size (256): the
result is left-zero-padded by `RSA_padding_check_none`, which returns `num`. So `res == 256`,
`zeroBytes == 0`, and the buggy `else if` block is never entered. It only bites a crypto
backend that returns a leading-zero-*stripped* length.

**Fix (1 line, defense-in-depth):** match `encrypt` → `subspan(zeroBytes, res)` (and
`subspan(0, zeroBytes)`), or assert `res == kDecryptSize`. Not a shippable vuln on current
backends, but it is a real correctness bug and should not be left as a trap.

---

## CRITICAL reasoning notes (do not re-derive — these each cost real analysis time)

1. **`rpc_result` reads one prime past `end` — but it is SAFE.** `handleOneReceived`
   `mtpc_rpc_result` (`session_private.cpp:1848`) does `from[0]` when `from` may equal `end`
   after reading the 2-prime `reqMsgId`. It does **not** OOB, because the MTProto encrypted
   envelope mandates `paddingSize ≥ 12` (`:1338`), i.e. ≥3 primes of padding follow `end`
   inside the *same* decrypted allocation. Don't "fix" this as a bug; it relies on the
   padding invariant, which is checked earlier.

2. **The RSA #3 bug is dead code on our crypto libs.** Confirmed by the OpenSSL/BoringSSL
   `RSA_padding_check_none` contract (returns `num`, left-pads). Anyone re-triaging #3 will
   waste time trying to reach it on a stock build — it can't be reached there. Log it, fix
   it defensively, move on.

3. **tgcalls glue single-packet `uint16_t(from.Length())` truncation is harmless.**
   `Message.cpp:202` `Deserialize(rtc::CopyOnWriteBuffer, singleMessagePacket=true)` casts a
   `size_t` remaining length to `uint16_t`. Truncation can only make the value **≤** the
   real remaining bytes (low 16 bits), so `AppendData(len)` never over-reads; an over-long
   single packet just fails the "single message didn't fill the packet" check afterwards.
   Not a bug.

4. **tgcalls version is recent → skip deep upstream fuzzing.** Submodule `616810f`
   (2026-04-15). The productive edge is the tdesktop glue seams, not out-fuzzing OSS-Fuzz on
   WebRTC internals. Version-diff first; only harness a specific glue seam if the version
   check points somewhere concrete (it didn't).

5. **Two DoS surfaces are the *accepted* class, not new bugs.** `ungzip` (no output cap) and
   `vector_type::read` (unbounded `count` alloc) are malicious-server-gated memory-DoS, same
   as upstream ships and already cleared to alloc-DoS-only. Don't re-file them as
   memory-corruption. The one concrete action: cap `ungzip` like the v2 path already does.

---

## Coverage — paths traced and swept clean this session (evidence)

**MTProto**
- **TCP framing** `connection_tcp.cpp`: `readPacketLength` caps every variant
  (`kPacketSizeMax`, `ints<<2` bounded, `kInvalidSize` on junk); `readPacket`/`parsePacket`
  assert `size <= bytes.size()` and non-empty.
- **Encrypted intake** `session_private.cpp:1275` `handleReceived`: `intsCount` range-checked
  `[kMinimalIntsCount, kMaxMessageLength/4]`; `messageLength` bounded and `%4`; `end` inside
  the decrypted buffer; msg_key SHA256 verified before interpretation.
- **Container / rpc_result** `:1472` / `:1824`: per-message `from+4 >= end` and
  `otherEnd > end` guards; `bytes.v` checked `≥4` and `%4`. (See note #1 for the safe
  past-`end` read.)
- **DH key exchange** `mtproto_dc_key_creator.cpp:587` `dhParamsAnswered`: `encDHLen` `%4` +
  `encDHBufLen ≥ 6`; `end = from + (encDHBufLen-5)` is exactly one-past the buffer; SHA1
  subspan bounded by consumed `(to-from)`.
- **TL primitives** `lib_tl/tl/tl_basic_types.h`: `string_type::read` bounds every branch via
  `HasBytes`; `Reader<mtpPrime>::Has/HasBytes/Get` all check `end - from`.
- **Post-deser server-message handlers** `session_private.cpp:1537-1822` (msgs_ack,
  bad_msg_notification, bad_server_salt, msgs_state_info/req, msg_resend, msgs_all_info,
  new_session_created, pong): all use bounded generated `read()`; the one raw-index spot,
  `handleMsgsStates` (`:2201`), rejects `states.size() != idsCount` before `states[i]`. Safe.
- **Received-ids window** `mtproto_received_ids_manager.cpp`: `std::map`-based dedup
  (`min`/`max`/`shrink` via map iterators) — no raw indexing, no OOB.
- **Flags/math** reviewed per request: `flags_type::read` bounds the 32/64-bit read via
  `Has(1/2)`; container `bytes.v >> 2` is `%4`+`≥4` checked and the `otherEnd > end` guard
  catches pointer overshoot on 64-bit. No unit/index defect beyond F-2 (bytes-vs-prime) and
  the latent RSA offset (#3).
- **DoH / special-config** `mtproto_domain_resolver.cpp`, `special_config_request.cpp`: DNS
  JSON via Qt `QJsonDocument` (bounds-safe); config blob size-gated (base64 exactly 344 →
  256 bytes); `decryptSimpleConfig` subspans derived from fixed sizes; `realLength` validated
  `(0, dataSize]` and `%4` with a trailing-length cross-check. (Clean apart from bug #3.)
- **HTTP transport** `connection_http.cpp`: Qt owns framing/Content-Length; post-read guards
  `size % 4` and `size >= 8`.

**tgcalls glue** (`616810f`, 2026-04-15)
- **`EncryptedConnection.cpp`**: intake gated `size ∈ [21, 128 KiB]`; msgKey SHA256 verified
  before parse; `processPacket`/`processRawPacket` loop always has ≥1 byte at `*reader.Data()`
  and requires ≥5 before continuing (consuming 4) → no empty-reader deref; each message
  consumes ≥1 byte → no infinite loop; replay/counter window bounded.
- **`Message.cpp`**: string len `< kMaxStringLength` (64 KiB) and bounds-checked; vector/format
  counts are `uint8` (≤255); `Deserialize(CopyOnWriteBuffer)` checks `from.Length() < length`
  before `AppendData`; raw-message len capped `≤ 1 MiB` with `ReadBytes` check. (See note #3.)
- **`NetworkManager.cpp:347`** entry gate → `handleIncomingPacket` rejects `size < 21` before
  dereferencing; **`CryptoHelper.cpp`** is fixed-size only.
- **Group `AudioStreamingPartInternal.cpp`**: `readInt32` checks `offset + 4 > length` before
  each `memcpy`; the `count`-driven loop bails via `readInt32` when data is exhausted → no
  unbounded index/alloc. Media bytes then flow into ffmpeg (out of scope: fuzzed upstream).
- **v2 `DirectNetworkingImpl.cpp:267`** UDP parser: `ReadUInt32` guarantees `size ≥ 4` before
  the `size()-4` subtraction (no underflow); `memcpy(...,12)` guarded by `size ≥ 12`;
  `dataSize` bounds-checked before `handleIncomingPacket`. v2 signaling caps gunzip at 2 MiB.

---

## Fuzzing campaign — MTProto TL deserializer (this session)

Harness + build live in `fuzz/` (mirrors the `dev/microtex-fuzz` pattern):
- `fuzz_tl.cpp` — libFuzzer harness. Copies fuzz bytes into an aligned heap
  `mtpPrime` buffer (ASan red-zones bracket it → past-end reads detected) and calls
  boxed `read(from,end)` on a selector-chosen server-reachable top-level type:
  `MTPUpdates`, `MTPmessages_Messages`, `MTPUser`, `MTPPage`, `MTPRichText`,
  `MTPWebPage`, `MTPhelp_ConfigSimple`, `MTPDataJSON` (recursive rich-text types
  included for stack-depth bugs). Defines the otherwise-undefined
  `base::assertion::log` so `Expects`/`Assert` failures null-deref → ASan catches them.
- `build_tl.sh` — compiles the real generated `out/Telegram/gen/scheme.cpp` (4.5 MB)
  + `tl_basic_types.cpp` + harness against **system Qt6 6.11.1**, `-std=c++20`
  (needed for `std::remove_cvref_t`), `-fsanitize=address,fuzzer`. Include roots:
  gen, SourceFiles, lib_tl, lib_base, lib_rpl, GSL, range-v3, expected.

**Empirical result — the known alloc-DoS reproduces instantly.** Unmodified, the
harness OOMs within seconds: a `tl::vector_type::read` `count` field of ~0xdadada…
drives `QVector<T>(count, T())` before any element is read (`tl_basic_types.h:552`).
This is the previously-accepted unbounded-count alloc-DoS — now confirmed by fuzzing,
not just by eye. (libFuzzer reports it as `out-of-memory`, artifact saved.)

**Harden-the-harness to reach deeper code** (microtex lesson): `fuzz/override/tl/tl_basic_types.h`
is a copy with one FUZZ-ONLY bound — reject `count > (end - from)` before the alloc
(a vector of N elements needs ≥ N primes of payload, so a larger count is
unsatisfiable). Compiled in via `-Ioverride` ahead of `lib_tl`. **This is also the
shape of the real fix** the product should adopt for the alloc-DoS. With it, RSS is
flat (~60 MB/proc) and the fuzzer explores the generated deserializers.

**Run config (memory-safe, small core count — freezes/OOM avoided):**
`fuzz/run_campaign.sh` → `-fork=4 -ignore_ooms=1 -ignore_timeouts=1 -rss_limit_mb=2048
-malloc_limit_mb=512 -timeout=10 -max_total_time=2400`. 4 parallel procs, total RSS
~1.8 GB, ~30k exec/s/proc.

**Result:** reached cov ≈ 6.4k features / ~200M executions, corpus ~1.9k, **0 heap
memory-corruption crashes** (confirms the generated read path is bounds-safe once the
alloc-DoS is bounded out). The campaign flagged **2 `-timeout` artifacts** — investigated:
they run <1 s standalone with baseline RSS (spurious timeouts from core oversubscription
during the run), but their bytes are chains of `textBold#6724abc4`, i.e. **the mutator
discovered `RichText` self-nesting**. Scaling the depth up reproduces a real
**stack-overflow → finding #5** (`findings/richtext_recursion/`). This is the fuzzer's
concrete payoff this session: an unbounded-recursion DoS in the TL deserializer that
eyeballing the (individually bounded) primitives would not have surfaced.

Scope note: this harness fuzzes the **generated TL read path** (vector/string/nested
boxed deserialization) — the codegen'd half of the "post-deserialization
interpretation" surface. The **hand-written** framing (container / rpc_result / gzip
in `session_private.cpp`, the FakeTLS hello state machine) is not reachable standalone
without the session/crypto scaffolding and was cleared by static tracing instead; a
future harness could stub the AES/SHA gate and drive `EncryptedConnection` /
`DeserializeMessage` (tgcalls) — expected yield low given the static trace.

---

## NEXT STEPS (continue here)

1. **Land the two working-tree fixes** (bugs #1, #2) with regression tests:
   - not-secure response: `answerLen` sub-prime, and `answerLen` far larger than body → span
     must be the real body / rejected, no OOB.
   - FakeTLS: Server Hello with `_serverHelloLength` below the digest region → rejected, no
     read/write past `_incoming`.
2. **Fix bug #3 defensively** (`subspan(zeroBytes, res)` or `assert(res == kDecryptSize)`).
3. **Cap `ungzip` output** (e.g. a few MiB, or reuse the v2 `gunzipData(..., cap)` pattern) to
   close the zip-bomb memory-DoS with parity to the v2 path.
4. Not yet examined (lower priority, if a later pass wants them): `mtproto_dc_key_binder.cpp`
   / `mtproto_bound_key_creator.cpp` (bind-temp-key server response parse);
   `SctpDataChannelProviderInterfaceImpl.cpp` (thin glue over usrsctp — real parse is
   upstream); group `VideoStreamingPart.cpp` / `AVIOContextImpl.cpp` (ffmpeg-backed).
5. If a harness is ever wanted: the tractable seam is `EncryptedConnection` /
   `Message.cpp` `DeserializeMessage` driven with a fixed key (bypass the AES/SHA gate by
   feeding already-"decrypted" buffers) — but the static trace already shows it bounded, so
   expected yield is low.

---

## Addendum — fresh pass over the previously-unexamined seams (2026-07-11, iteration #2)

This pass closes the four "not yet examined" items from NEXT STEPS #4 plus the
**proxy / obfuscated-transport** path the task explicitly called out
(`core.telegram.org/proxy`). Method: static trace of each read boundary. **Net-new
memory-corruption defects: 0.** All four seams are bounds-safe; evidence below.

### A. Obfuscated TCP transport + MTProto-proxy secret (`connection_tcp.cpp`, `mtproto_proxy_data.cpp`) — clean

The obfuscated ("dd"/"ee"/secret) transport and the proxy-secret parser were traced
end-to-end because they sit on the pre-auth wire and the task flagged the proxy angle.

- **Threat-model note:** the MTProto-proxy *secret* is **user-supplied** (the proxy the
  user pastes/imports), not attacker-supplied over the network. `ProxyData::secret*` and
  `ValidateSecret` only gate config the user already chose to trust. So the proxy secret
  is not an unauthenticated-remote input; the *server/proxy bytes on the socket* are.
- `TcpConnection::Protocol::Version0::readPacketLength` (`:104`): the `0x7F`-tagged path
  reads a 3-byte length, computes `(ints<<2)+4`; `ints ≤ 0xFFFFFF` so the shift stays
  well under `INT_MAX` (no signed overflow). `readPacket` (`:126`) `Assert`s
  `size <= bytes.size()` before the subspan, and the smallest legal packet is ≥ 4 bytes,
  so `parsePacket`'s `Assert(!ints.empty())` (`:397`) cannot fire from a short packet.
- `VersionD::readPacketLength` (`:207`): guards `bytes.size() < 4` first, then
  `value = *(uint32*)data + 4` is range-checked `[8, kPacketSizeMax)` → `kInvalidSize`
  otherwise. Wrap (`raw == 0xFFFFFFFF → value 3`) fails the `≥ 8` gate. Clean.
- `parsePacket` (`:391`): `packet.size() / sizeof(mtpPrime)` truncates a non-4-multiple
  length **down**, and the following `memcpy` copies `ints.size()*4 ≤ packet.size()`
  bytes — no over-read of the decrypted span.
- AES-CTR de-obfuscation (`socketRead` `:312`, `prepareConnectionStartPrefix` `:446`) is
  a fixed keystream XOR over exactly the bytes read; it carries no length field and
  cannot itself over-run. Key/iv are derived from a locally-generated 64-byte nonce.

### B. Group-call `VideoStreamingPart` container parser (`group/VideoStreamingPart.cpp`) — clean

Server-supplied broadcast/live-stream part bytes (reachable when watching a group
call / RTMP stream) are parsed by a hand-written TLV reader before the ffmpeg handoff.

- `readInt32` (`:120`) / `readBytesAsInt32` (`:132`) both check `offset + n > data.size()`
  before `memcpy`; `count` is clamped to `(0,4]`. `readSerializedString` (`:164`) validates
  `offset + length > data.size()` before constructing the `std::string`, and the 3-byte
  `254`-prefixed length maxes at `0xFFFFFF` (no `offset+length` `int` overflow for any
  realistic part size).
- `VideoStreamingPartState` ctor (`:736`): every slice is fenced — `events[i].offset < 0`
  skipped, `endOffset` taken as `data.size()` (last) or `events[i+1].offset` assigned to a
  `size_t` (a negative int32 becomes huge → caught by `endOffset > data.size()` → skip),
  and `endOffset <= events[i].offset` skipped. So `data.begin()+offset` /
  `data.begin()+endOffset` are always in `[begin,end]`. No OOB slice.
- Note: `consumeVideoStreamInfo` (`:250`) reads **only one** event even when `eventCount>1`
  (no loop), so the multi-event `events[i+1]` indexing is effectively dead — narrows the
  surface further. Decoded bytes then flow into ffmpeg via `AVIOContextImpl` (out of scope:
  fuzzed upstream). Minor non-safety robustness nit: `avcodec_alloc_context3` /
  `avcodec_parameters_copy` returns are unchecked (OOM-only null-deref, not attacker-driven).

### C. `DcKeyBinder::handleResponse` (`details/mtproto_dc_key_binder.cpp:107`) — clean

Bind-temp-key server reply parse. `Expects(!response.isEmpty())` guards the `response[0]`
tag read; the only variable-length branch is `MTPRpcError::read(from, end)`, the
bounds-checked generated deserializer, with `end = from + response.size()`. No raw
indexing, no unit mismatch. Safe.

### D. `SctpDataChannelProviderInterfaceImpl` (tgcalls) — clean (thin glue)

`OnMessage` (`:90`) copies `buffer.data` into a `std::string` by explicit begin/end
pointers (size-correct, no length field). `OnDataReceived`/`SendData` forward straight to
webrtc's `SctpDataChannel` — the actual SCTP chunk parsing lives in upstream
usrsctp/webrtc (out of scope: heavily fuzzed). No tdesktop-side parse to get wrong.

**Iteration-#2 verdict:** the four remaining seams join the "swept clean" column. The
substantive findings for this focus area remain the five already documented above
(M-1/M-2 fixed; RSA-offset latent/dead; the two unbounded-recursion DoS). No new
memory-corruption primitive surfaced; consistent with the crypto-core being correct and
the residual risk being the accepted malicious-server-gated alloc/recursion DoS class.

## Addendum — iteration #5: independent verification of the newer items (2026-07-11)

This pass independently re-derived the two items added after the fixes were committed
(**O-1** framing clamp and **F-1** stale FFmpeg) against the live tree, because they were
not in the original sessions and a release gate should not rest on an unverified claim.
Both hold; precise evidence and one nuance below.

### F-1 (stale FFmpeg via group-call `container`) — CONFIRMED, and this is the sharpest tgcalls exposure

- **Version pinned = FFmpeg `n6.1.1`**, verified at `Telegram/build/prepare/prepare.py:1178`
  (`git clone -b n6.1.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg`, then
  `patches/ffmpeg.patch`). n6.1.1 is a late-2023 point release — **~1.5–2 years stale** as of
  this review, so every demuxer/parser CVE fixed upstream since then is carried unpatched.
- **Reachability is real and attacker-selectable.** In `group/VideoStreamingPart.cpp` the
  container string comes from `consumeVideoStreamInfo` → `readSerializedString` (`:238`),
  i.e. **fully server/stream-peer-controlled**, and is passed verbatim to
  `av_find_input_format(container.c_str())` (`:493`). So a malicious group-call/broadcast
  stream chooses *which* FFmpeg demuxer parses its bytes and supplies those bytes via
  `AVIOContextImpl`. This is semi-0-click once the victim is in / watching the call.
- **Nature:** stale-dependency reachability, **not** a tdesktop glue bug — the glue (the TLV
  slicer ahead of it) is bounds-safe (see iteration-#2 §B). Severity depends entirely on the
  specific n6.1.1→HEAD demuxer CVE deltas; a concrete score needs a version-diff of the
  *enabled* demuxers against the FFmpeg security advisories, which is the right next step.
- **Recommended fixes (two, complementary):** (1) bump the pinned FFmpeg to a current
  release and re-apply the patch; (2) defense-in-depth — **allowlist `container`** to the
  small known-good set the group-call protocol actually uses before calling
  `av_find_input_format`, so a hostile stream cannot pivot to an obscure demuxer. (2) is a
  cheap, local, buildable change even before the bump lands.

### O-1 (`Version0::readPacketLength` lacks the `kPacketSizeMax` clamp) — CONFIRMED but minor

- True as stated: `connection_tcp.cpp:118` returns `int(ints << 2) + 4` for the `0x7F` path
  with `ints` a 24-bit field (max `0xFFFFFF`), with **no** upper clamp, whereas `VersionD`
  gates `value < kPacketSizeMax` (`:213`). A malicious server/proxy can thus request a
  single ~**64 MiB** buffer allocation per packet via `ensureAvailableInBuffer(packetSize)`.
- **Nuance worth recording:** the practical differential is small. `kPacketSizeMax =
  0x01000000 * 4 = 64 MiB` (`:20`), so `VersionD` *also* permits ~64 MiB — the clamp mostly
  rejects the `≥ 64 MiB` tail, not a materially smaller ceiling. And the non-`0x7F` Version0
  path maxes at `0x7E << 2 = 504` bytes. So O-1 is a **bounded single-alloc DoS** (≤~64 MiB,
  transient, per-connection), MITM/proxy-gated — a 1-line hardening nit (`return (ints ... &&
  (int(ints<<2)+4) < kPacketSizeMax) ? ... : kInvalidSize`), not a corruption bug. Keep it
  Low.

### Net

Iteration #5 changed no verdict: the two committed pre-auth fixes (#1/#2) remain correct in
the tree (`4467e0cbf1`), and every open item stays DoS-class or dead-code, exactly as the
consolidated TL;DR states. The one item worth *escalating operationally* before release is
**F-1** — not because of a new glue bug, but because a 2-year-old FFmpeg reachable through an
attacker-chosen demuxer name is the highest-EV surface in this whole review, and closing it
is a dependency bump + a one-line allowlist rather than a code-safety fix.

## Key file:line references (verified this session)

MTProto
- `connection_abstract.cpp:141` — `parseNotSecureResponse` (bug #1 fix site).
- `connection_abstract.h:197` — `prepareNotSecurePacket` writes `messageLength << 2` (bytes).
- `connection_tcp.cpp:104-134,207-226` — `readPacketLength`/`readPacket` (framing caps).
- `connection_tcp.cpp:391-409` — `parsePacket` (`ints.size()` guards).
- `session_private.cpp:1275-1452` — `handleReceived` (decrypt + envelope checks).
- `session_private.cpp:1454-1535` — `handleOneReceived` gzip/container.
- `session_private.cpp:1824-1888` — `mtpc_rpc_result` (safe past-`end` read, note #1).
- `session_private.cpp:2025-2075` — `ungzip` (uncapped output; TODO cap).
- `details/mtproto_tls_socket.cpp:757` — `checkHelloDigest` (bug #2 fix site).
- `details/mtproto_dc_key_creator.cpp:587-679` — `dhParamsAnswered` (DH inner-data parse).
- `details/mtproto_rsa_public_key.cpp:146-162` — `decrypt` (bug #3) vs `encrypt` `:128-144`.
- `special_config_request.cpp:422-486` — `decryptSimpleConfig` (bug #3 caller).
- `lib_tl/tl/tl_basic_types.h:399-435,547-562` — `string_type::read` / `vector_type::read`.
- `core_types.h:282-315` — `Reader<mtpPrime>::Has/HasBytes/Get`.

tgcalls
- `EncryptedConnection.cpp:119-155,407-441,478-562` — decrypt gate + `processPacket` loop.
- `Message.cpp:18-31,201-215,368-404` — string/buffer/raw deserializers.
- `NetworkManager.cpp:340-355` — `transportPacketReceived` entry gate.
- `CryptoHelper.cpp:8-59` — `PrepareAesKeyIv` / `AesProcessCtr` (fixed-size).
- `group/AudioStreamingPartInternal.cpp:47-97` — `readInt32` / `parseChannelUpdates`.
- `v2/DirectNetworkingImpl.cpp:267-338` — `processIncomingPacket` UDP parser.
- `v2/InstanceV2Impl.cpp:1634-1687` — `receiveSignalingData` (gunzip cap 2 MiB @1681).
