# tgcalls / MTProto Robustness & Memory-Safety Review — Session Findings

Date: 2026-07-11. Branch: `test/collapsed-chats-alert-bar`.
Target: Telegram Desktop fork — **tgcalls / lib_webrtc call transport** and
**MTProto transport / TL deserialization**, the network-facing byte handlers we
want clean before release.

Scope this pass = *static tracing of the read/deserialize paths* (bytes → gate →
handler), calibrated to the honest threat model: both areas need a
malicious/MITM server, a malicious proxy, or a negotiated call to reach; the
realistic ceiling is DoS/logic, not remote memory corruption. No source files
were modified — this is analysis only.

---

## TL;DR — iteration #3 (protocol / proxy deep pass) — **F-2: pre-auth OOB read**

**F-2 (HIGH — the real memory-safety bug this project set out to find): a
unit-confusion in the MTProto plaintext-handshake parser lets a malicious server
/ MITM / MTProto proxy drive an out-of-bounds heap read, pre-authentication, on
the very first handshake reply.**

- **Where / reach (TWO consumers, both pre-auth):**
  `AbstractConnection::parseNotSecureResponse` (`connection_abstract.cpp:107-150`)
  returns the over-long span; both callers then do
  `result.read(from, from + answer.size())` with the inflated size:
  1. `readPQFakeReply` (`connection_abstract.cpp:158`) — the transport liveness
     probe `Req_pq → ResPQ`, from `connection_tcp.cpp:602` /
     `connection_http.cpp:210`. Runs on **every** connection.
  2. `DcKeyCreator::answered → readNotSecureResponse` (`dc_key_creator.cpp:455,435`)
     — the **actual MTProto 2.0 auth-key exchange** (ResPQ / Server_DH_Params /
     Set_client_DH_params_answer). So the whole plaintext handshake is exposed,
     not just the probe.
  Both are before any auth key exists → reachable by a malicious DC, a MITM, or a
  malicious/compromised **MTProto proxy** (`core.telegram.org/proxy`).
- **Root cause (the "math/units" bug):** the length field is produced in
  **bytes** — `prepareNotSecurePacket` writes
  `*messageLength = (primeCount + padding) << 2` (`connection_abstract.h:197`),
  and the bound check treats it as bytes:
  `answerLen > (len - 5) * sizeof(mtpPrime)` (`:133`). But `answer` is a
  `const mtpPrime*` (int32) and the original tail passed `answerLen` straight to
  `gsl::make_span(answer + 5, answerLen)` — where the second arg is an **element
  (int32) count**. So the span was up to **4× longer than the packet**:
  end = `answer + 5 + answerLen_bytes` vs the real buffer end `answer + len`,
  overrunning by up to `~3·len` int32s. The returned span's `size()` is then the
  TL read boundary (`response.read(from, from + answer.size())`, `:166`), so the
  `MTPResPQ` deserializer reads past the heap allocation.
- **Impact = reliable remote pre-auth DoS (confirmed).** The crash primitive is
  the TL `vector<long>` read of `server_public_key_fingerprints`
  (`tl_basic_types.h:547-559`): it reads `count`, then loops `count` longs, each
  `Has(2,·,end)`-checked against the **inflated** `end`, so the loop walks toward
  `answer + 5 + answerLen ≈ answer + 4·len`. With an attacker-sized large
  plaintext packet (framing allows up to ~64 MB, `parsePacket` sizes the buffer
  straight from it, `connection_tcp.cpp:406`) the overrun is **~3·len primes
  ≈ megabytes past a large mmap'd heap block → an unmapped page → SIGSEGV**,
  reliably. `poc_respq_dos.cpp` (1 MB packet) computes overrun = **~3.1 MB** and
  ASan faults inside the vector-walk `Get`; the fixed variant reports overrun = 0.
  It is an OOB **read**, not a write, and the parsed values are consumed
  internally (a wrong fingerprint just fails the handshake) → **no RCE, limited
  heap exfiltration; the realistic impact is a remote crash / DoS.**
- **Reproducibility:** deterministic. `bash dev/tgcalls-mtproto-audit/repro.sh`
  builds both PoCs and shows buggy→ASan-abort (nonzero exit), fixed→clean (0).
  This is a *demonstrated* crash, unlike F-1 (a version-diff exposure, not a
  reproduced ffmpeg crash).
- **Fix (present in the working tree, verified correct):**
  `gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime))` — converts the byte
  length to a prime count so the span ends exactly at `answer + len`. Safe on
  malformed non-multiple-of-4 lengths too (integer division floors → reads fewer,
  never more). `readPQFakeReply` is the only consumer of the span, so the fix is
  complete.
- **PoC (demonstrated, not just argued):** `poc_notsecure_oob.cpp` replicates the
  exact length logic on a heap `std::vector<int32>` and walks the returned span
  like the TL reader does. A 16-prime buffer with `answerLen = 44` (= max allowed
  by the byte-bound check) makes the **buggy** variant read 44 primes from offset
  5 → 33 primes past the allocation:
  ```
  ==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 4
      #0 consume(...) poc_notsecure_oob.cpp:33
  ```
  The **fixed** variant (`answerLen / 4`) runs clean. Build:
  `clang++ -std=c++17 -g -fsanitize=address poc_notsecure_oob.cpp -o poc && ./poc`
  (buggy) vs `./poc fixed`.
- **Regression test (TODO in-tree):** port the PoC as a unit test on
  `readPQFakeReply` asserting the span size is `(len-5)` primes, run under ASan.

---

## TL;DR — iteration #2 (deep pass: harness + CVE version-diff)

**F-1 (elevated — the real finding): pinned FFmpeg `n6.1.1` is stale, and the
group-call video streaming path hands an _attacker-controlled container name_ to
`av_find_input_format()`, exposing the full set of compiled-in demuxers to a
malicious group-call broadcaster.** This is the "reachable known-CVE through our
glue" case the brief prioritizes.

- **Reachability (traced):** `VideoStreamingPart.cpp:238` reads `container` from
  the wire via `readSerializedString`, then `:493`
  `av_find_input_format(container.c_str())` + `avformat_open_input` over the
  attacker's bytes (`AVIOContextImpl`). So the sender picks **which demuxer**
  parses their bytes. Enabled & reachable demuxers (from
  `build/prepare/prepare.py:1210+`): **matroska, mov, m4v, ogg, wav, gif, aac,
  flac, h264, hevc, mp3**; transitively every enabled decoder (hevc, h264, av1,
  vp8/9, mpeg4, msmpeg4v2/3, aac, opus, vorbis, gif, many pcm…). Audio path is
  narrower (container hard-coded `"ogg"`, `StreamingMediaContext.cpp:914`).
- **Staleness (measured, full range):** `FFmpeg n6.1.1` (Nov 2023) vs upstream
  `release/6.1` = **930 commits behind**. Enumerated all 930: **25 memory-safety
  fixes land in enabled+reachable modules the client is missing**, concentrated
  in the two most-exposed demuxers:
  - `avformat/mov` (×7): edit-list overflow, dts overflow, 64-bit CENC subsample
    bounds, "do not allocate out-of-range buffers", integer overflow in parser,
    non-existing-fragment crash.
  - `avformat/matroskadec` (×5): `num_levels` non-negative, `pre_ns` overflow,
    bound `TRACKENTRY` by `max_streams`, signed overflow in DASH cue diffs.
  - `avformat/oggparse*` (×3), `avformat/wavdec` (×1) — all enabled demuxers.
  - decoders: `vp9 recon` buffer overflow, `vp8dsp` OOB, `flac_parser` overrun,
    `mpeg4videodec`, `h264_parser`/`h264_slice` (chroma underflow), `libvorbisdec`
    /`oggparsevorbis` integer overflow, `av1dec`, `cbs_av1` shift overflow.
  (Query: `GET /repos/FFmpeg/FFmpeg/compare/n6.1.1...release/6.1`, filtered to
  enabled modules — reproducible.)
- **Severity, honest:** requires a malicious group-call server/broadcaster
  (semi-0-click once you're in a group call); impact = whatever each missing
  fix is (mostly DoS, some heap-OOB). Not novel bugs — a **stale-dependency
  reachability** finding. **Fix = bump the pinned FFmpeg to current `release/6.1`
  (or 7.x) and, defensively, allowlist the `container` string to the set the
  server is actually expected to send** instead of passing it raw to ffmpeg.

**Dynamic confirmation (harness, not just reading):** built a libFuzzer+ASan+UBSan
harness over the exact `consumeVideoStreamInfo` header parser
(`fuzz_videostream.cpp`, functions copied verbatim). **22.6M executions, 0
crashes**, coverage saturated → the hand-rolled tgcalls readers are bounds-safe;
the risk is downstream in ffmpeg (F-1), not in the glue parser.

Two harnesses this pass, ~65M executions total, **0 memory-safety crashes** in
tdesktop glue:
- `fuzz_videostream.cpp` — `consumeVideoStreamInfo` (22.6M execs).
- `fuzz_mtp_length.cpp` — MTProto TCP `Version0`/`VersionD` length parsers, the
  proxy/MITM-facing seam, asserting `sizeLength <= size` (no subspan underflow);
  42.7M execs, clean. Confirms **O-1** is a bounded-alloc DoS, not memory unsafety.

Build/run:
```
cd dev/tgcalls-mtproto-audit
clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined fuzz_videostream.cpp -o fuzz_videostream
ASAN_OPTIONS=detect_leaks=0 ./fuzz_videostream -max_total_time=45 vs_corpus vs_seeds
clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all fuzz_mtp_length.cpp -o fuzz_mtp_length
ASAN_OPTIONS=detect_leaks=0 ./fuzz_mtp_length -max_total_time=30 ml_corpus
```

**Negatives worth recording (ruled out this pass):**
- **WebRTC / tg_owt is NOT stale.** Pin `89df288d` (2026-04-09) sits exactly at
  `desktop-app/tg_owt` master (0 commits behind). So the large upstream WebRTC
  surface — including the SCTP/dcSCTP stack — carries no *stale*-CVE exposure
  here; it tracks current. (Contrast F-1: the FFmpeg pin is the outlier.)
- **SCTP data-channel glue is a thin forwarder.** `SctpDataChannelProviderInterfaceImpl.cpp`
  does no length arithmetic — it hands bytes straight to the (current) webrtc
  `SctpDataChannel::OnDataReceived` and delivers text via a callback. No
  glue-level parsing bug; the real SCTP chunk parsing lives in up-to-date webrtc.

---

## TL;DR — iteration #1 result

**No memory-corruption defect found in the traced seams.** The tgcalls transport
framing, message deserialization, custom AES-CTR glue, and the MTProto TCP
length parsing are all bounds-checked before allocation/indexing. This is a
*negative result with evidence*, not an unfinished pass — the code that would
have carried a classic length-field OOB is guarded at each choke point.

Residual observations (all **DoS-class or logic**, all gated behind a
malicious/MITM peer or proxy — worth a defensive tightening but not shippable
vulns):

1. **O-1 (low / alloc-DoS):** MTProto abridged transport (`Version0`) extended
   length (`0x7F` marker) can request a ~64 MB packet buffer with **no
   `kPacketSizeMax` clamp**, unlike `VersionD` which clamps. Bounded allocation,
   MITM/proxy-controlled. `connection_tcp.cpp:118`.
2. **O-2 (info / logic):** `consumeVideoStreamInfo` reads an `eventCount` but
   only ever parses **one** event, ignoring the count. Not a safety bug; a
   spec/robustness mismatch worth noting. `group/VideoStreamingPart.cpp:250-262`.
3. **O-3 (info / UB-adjacent):** hand-rolled `readInt32(...int &offset)` uses a
   signed `int` offset compared against `size_t` size; safe only because input
   sizes stay far below `INT_MAX`. Defensive: make offset `size_t`.
   `group/VideoStreamingPartInternal`/`AudioStreamingPartInternal.cpp`.
4. **O-4 (known / abort-DoS):** MTProto TL primitives (`Reader<mtpPrime>`) enforce
   bounds via `Expects()` → abort, i.e. the "already cleared to alloc/abort-DoS"
   class the brief calls out. Confirmed still the case. `core_types.h:283-315`.

Vendored `tgcalls` = commit `616810f1` (2026-04-15, `ios-release-11.13-16`) —
recent; no obviously-stale component surfaced for a cheap known-CVE hit this
pass. WebRTC/ffmpeg version-diff vs CVEs = **next iteration** (see below).

---

## What was traced, and why it's clean (evidence)

### tgcalls transport crypto + framing — `EncryptedConnection.cpp`
The MTProto2-style envelope: `msgKey = SHA256(key_fragment || plaintext)[8:24]`,
AES-CTR body. Decrypt path recomputes msgKey over the decrypted plaintext and
compares in constant time before interpreting anything.

- Size gate up front: `handleIncomingPacket` / `decryptRawPacket` require
  `21 <= size <= kMaxIncomingPacketSize (128 KB)` (`:120`, `:409`, `:444`).
- `processPacket` / `processRawPacket` loop invariant holds: the top-of-loop
  `uint8_t(*reader.Data())` (`:496`, `:582`) is only reached with `reader`
  non-empty — first iteration has `fullBuffer.size()-4 >= 1` (asserted
  `fullBuffer.size() >= 5`), and continuation requires `reader.Length() != 0`
  then `>= 5` then consumes a 4-byte seq, leaving `>= 1`. No unchecked deref.
- Key-material reads stay in-bounds: max offset `key + 88 + x + 32` with
  `x <= 136` → `<= key+256`, and the auth key is 256 bytes.

### Message deserialization — `Message.cpp`
Every reader is length-checked before use:
- `Deserialize(std::string&)` rejects `length >= kMaxStringLength (65536)` and
  relies on `ReadString`'s own bounds (`:18-31`).
- `DeserializeRawMessage` clamps `length <= 1 MB`, `SetSize`, then a
  bounds-checked `ReadBytes` (`:381-404`).
- Nested counts (`SdpVideoFormat` params, candidates, formats) are `uint8_t` and
  each element read is itself failure-checked; `encoders > formats.size()`
  rejected (`:185`). `TryDeserialize` asserts non-empty and matches on the id
  byte before `Consume(1)` (`:297-311`).
- Only nit: `Deserialize(CopyOnWriteBuffer&, singleMessagePacket=true)` truncates
  remaining length via `uint16_t(from.Length())` — masked because the signaling
  cap (16 KB) and transport cap (~1.4 KB) are both `< 65536`, so no truncation is
  reachable. Documented, not exploitable.

### MTProto TCP framing — `connection_tcp.cpp`
- `VersionD::readPacketLength`: `uint32 + 4` overflow is caught by the
  `value >= 8 && value < kPacketSizeMax (64 MB)` gate (`:212-215`).
- `readPacket` asserts `size <= bytes.size()`; the state machine only dispatches
  a packet once `available.size() >= packetSize` (`:349`), else grows the buffer
  deliberately. No OOB subspan.
- **O-1** lives here: `Version0` (abridged) `0x7F` path returns
  `int(ints << 2) + 4` (up to ~64 MB) with only `ints >= 0x7F` checked — no
  `kPacketSizeMax` clamp — so a MITM/proxy can drive a single ~64 MB allocation
  via `ensureAvailableInBuffer`. Same magnitude as the VersionD cap, so
  low-severity, but asymmetric and worth clamping for consistency.

### Group-call media parse — `group/AudioStreamingPartInternal.cpp`, `VideoStreamingPart.cpp`
Hand-rolled parsers over base64/TL-ish blobs feeding ffmpeg. Every `readInt32` /
`readSerializedString` / `readBytesAsInt32` bounds-checks against the buffer
before `memcpy`/`std::string` construction; channel count capped at 8; the final
`data.erase(begin, begin+offset)` is reached only when the last read passed its
`offset+4 <= size` gate, so `offset <= size`. O-2/O-3 noted above are the only
smells; both benign at real input sizes. Deep codec internals = ffmpeg, upstream
-fuzzed, explicitly out of scope.

### JSON join payload — `group/GroupJoinPayloadInternal.cpp`
Parsed via `json11` with type-checked `absl::optional` accessors; no raw length
arithmetic. Safe.

---

## Protocol / crypto layer trace (voice-call E2E + MTProto DH) — verdict: correct

Traced the actual key-agreement, not just framing. Result: **matches the upstream
official implementation; no downgrade/commitment/validation gap found.**

- **DH config validation** — server-provided `p`/`g` go through `IsPrimeAndGood`
  before use: 2048-bit, `isPrime`, safe-prime `(p-1)/2` prime, and per-`g`
  residue checks; fast-path only for the pinned known-good prime.
  `mtproto_dh_utils.cpp:17-131`, gated at `calls_instance.cpp:542`. `random` length
  forced to 256 (`calls_instance.cpp:533-537`).
- **Public-value validation** — both `g_a` and `g_b` pass `IsGoodModExpFirst`
  (rejects `x < 2^(2048-64)` and `x > p - 2^(2048-64)`, i.e. small-subgroup /
  boundary values) inside `CreateAuthKey` before the shared key is derived.
  `mtproto_dh_utils.cpp:88-103,160-173`.
- **Commitment scheme** — callee commits to `g_a` via `g_a_hash` (SHA256, size
  checked = 32, `calls_call.cpp:744-752`); on confirm, `_gaHash == Sha256(g_a)`
  is enforced before deriving the key (`calls_call.cpp:1001-1006`). Prevents the
  caller from adaptively choosing `g_a` after seeing `g_b`.
- **Key fingerprint** — the incoming side recomputes `_keyFingerprint` from its
  own derived auth key and compares against the peer's server-relayed value
  (`calls_call.cpp:1301`); mismatch → call fails. MITM tamper is detected.

Net: the call E2E and MTProto2 auth math are textbook-correct here. The realistic
residual risk stays **post-deserialization / DoS**, exactly as the brief predicted.

## Methodology (reuse next iteration)

Pure static trace, no harness stood up this pass (the transport seams are small
and the invariants tractable by reading). The productive order was: (1) size gate
at the socket boundary, (2) the decrypt/authenticate step, (3) the per-message
`reader` loop invariant, (4) each `Deserialize` leaf's length check. A bug in
this class would show up as a count/length field used before a `HasBytes`-style
check — none found.

## NEXT STEPS (continue here — highest value first)

1. **Version-diff WebRTC + ffmpeg vs public CVEs** (the brief's best
   effort-per-value for tgcalls). `lib_webrtc` and the ffmpeg used by
   `AudioStreamingPartInternal`/`VideoStreamingPart` are the real upstream
   surface; identify the pinned versions and check for a known-CVE reachable
   through the group-streaming AVIO path (`AVIOContextImpl.cpp` feeds
   attacker-influenced bytes into `avformat_open_input`).
2. **Stand up a libFuzzer harness on two concrete leaves** now that they're
   isolated: `DeserializeMessage` / `DeserializeRawMessage` (feed raw
   post-decrypt bytes) and `consumeVideoStreamInfo` (feed the custom container
   header). Both are pure functions over a byte span — cheap to fuzz, and would
   confirm the static conclusion or surface an edge I missed.
3. **Clamp O-1**: add the `kPacketSizeMax` check to `Version0::readPacketLength`'s
   `0x7F` branch (defensive, one line) and add a regression test.
4. **SCTP data-channel seam** (`SctpDataChannelProviderInterfaceImpl.cpp`,
   `NetworkManager.cpp`) not yet traced — the data-channel path is a distinct
   parser worth a look before signing off.

## Iteration (later pass) — dynamic fuzzing of the group-stream leaf parsers + audio-path F-1 breadth

Closes NEXT-STEPS #2 (harness the isolated container/channel parsers) and adds one
reachability fact to F-1. Method: libFuzzer + ASan, multi-core `-fork`, headless.

**New harness `fuzz_audio_channels.cpp` (built + campaigned).** Extracts, verbatim, the
two parsers that were *never fuzzed* before plus the post-parse slice logic:
- **`parseChannelUpdates`** (`AudioStreamingPartInternal.cpp:59-97`) — the audio
  channel-update TLV, attacker-controlled `count` driving a 3×int32-per-iter loop.
- The **`VideoStreamingPartState` slice-bounds model** (`VideoStreamingPart.cpp:742-799`):
  the `events[i].offset` / `endOffset` fencing + `dataSlice` construction, with an ASan
  read-touch over each produced slice so any bad range faults.

**Campaign results (this pass):**
| harness | target | execs | cov (ft) | crashes/oom/timeouts |
|---------|--------|-------|----------|----------------------|
| `fuzz_audio_channels` (6 forks, 600s cap) | `parseChannelUpdates` + video slice model | **~193M** | 244 (saturated) | **0 / 0 / 0** |
| `fuzz_videostream` (4 forks, re-run) | `consumeVideoStreamInfo` | **~110M** | 276 (saturated) | **0 / 0 / 0** |

~**303M** total executions, **zero** memory-safety events; coverage plateaued early (these
are tiny, fully-explorable parsers). Dynamically confirms the static conclusion: the
hand-written group-stream readers are bounds-safe, including the audio channel-update path.
The `count`-driven loop cannot over-allocate — each iteration must consume 12 fresh bytes or
`readInt32` returns `nullopt` and the loop bails (bounded by `data.size()/12`).

**F-1 breadth correction — the stale-FFmpeg exposure is not video-only.**
`AudioStreamingPartInternal` (`:110`) passes the **same attacker-controlled `container`
string** to `av_find_input_format(container.c_str())` as the video path does (`:493`), and
feeds the payload via its own `AVIOContextImpl`. So a hostile broadcast stream reaches the
pinned FFmpeg `n6.1.1` demuxers through **both** the audio and video group-stream legs.
Reachability confirmed end-to-end: `StreamingMediaContext` fills `part.data` from the
`_requestAudio/VideoBroadcastPart` callbacks (`:898-902`, `:1001-1005`) — i.e. server/
CDN-directed MTProto broadcast-part fetches — then hands those bytes to these parsers.
Widen the F-1 fix (bump FFmpeg + **`container` allowlist**) to cover the audio demuxer
selection too, not just video.

**Audio PCM copy re-checked, safe.** `fillPcmBuffer` (`:335-351`): `_pcmBuffer`
(`vector<int16_t>`) is resized to `nb_samples * nb_channels` *elements*; the `S16` `memcpy`
copies `nb_samples * 2 * nb_channels` *bytes* = exactly the element byte-size; `nb_channels`
is rejected if `> 8` (`:327/329`). No unit mismatch, no overflow.

## Key file:line references (verified this session)
- `EncryptedConnection.cpp:119-155` — `decryptRawPacket` (size gate + MAC).
- `EncryptedConnection.cpp:478-562` — `processPacket` loop invariant.
- `Message.cpp:18-31,201-215,381-404` — string/buffer/raw deserializers (bounds).
- `Message.cpp:292-311` — `TryDeserialize` id-match dispatch.
- `CryptoHelper.cpp:8-27` — `PrepareAesKeyIv` (in-bounds key reads).
- `connection_tcp.cpp:104-124` — `Version0::readPacketLength` (O-1 at :118).
- `connection_tcp.cpp:207-226` — `VersionD::readPacketLength` (overflow-safe).
- `connection_tcp.cpp:20` — `kPacketSizeMax = 64 MB`.
- `core_types.h:283-315` — `Reader<mtpPrime>` (Expects/abort-guarded).
- `group/VideoStreamingPart.cpp:120-267` — custom container parse (O-2, O-3).
- `group/AudioStreamingPartInternal.cpp:47-97` — `parseChannelUpdates` (O-3).
</content>
