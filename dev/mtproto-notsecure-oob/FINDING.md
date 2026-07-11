# MTProto pre-auth OOB heap read in `parseNotSecureResponse`

**File:** `Telegram/SourceFiles/mtproto/connection_abstract.cpp:141`
**Class:** CWE-125 out-of-bounds read (memory safety), pre-authentication.
**Introduced:** upstream, commit `941288b58e` (2018) — also present in official Telegram Desktop.
**Reachability:** malicious/MITM server or a malicious MTProto proxy, at connection setup, before any auth key exists.

## Root cause — units mismatch (bytes vs. mtpPrime elements)

`parseNotSecureResponse()` reads the message length field and validates it as a **byte** count:

```cpp
const auto answerLen = (uint32)answer[4];                 // bytes on the wire
if (answerLen < 1 || answerLen > (len - 5) * sizeof(mtpPrime)) return {}; // bytes
return gsl::make_span(answer + 5, answerLen);             // BUG: element count
```

`answer` points at `mtpPrime` (`int32`) and `gsl::make_span(ptr, count)` treats `count`
as an **element** count. So a byte length is used as a prime count: the returned span —
and the `end` pointer both callers derive from it via `read(from, from + answer.size())`
— can extend up to **4× past** the real packet body, i.e. past the heap allocation.

Send side confirms `answer[4]` is bytes: `prepareNotSecurePacket` writes
`*messageLength = (...) << 2;` (`connection_abstract.h:197`).

## Consumers (both pre-auth handshake paths)

- `AbstractConnection::readPQFakeReply` — the fake `req_pq` probe sent on **every** TCP/HTTP
  connect (`connection_tcp.cpp:602`, `connection_http.cpp:210`).
- `DcKeyCreator::readNotSecureResponse<T>` / `handleAnswer` — the full DH auth-key creation
  exchange (`mtproto_dc_key_creator.cpp:435,455`).

Both do `result.read(from, from + answer.size())`; with the inflated `end`, every TL bounds
check inside `read()` is against the wrong boundary, so an attacker-controlled inner length
(a TL `bytes`/`string` length prefix, or a `Vector` count) makes the deserializer read past
the allocation.

## Impact

Out-of-bounds heap read, size and distance attacker-tunable via the length field plus the
inner TL length. Realistic impact: a remotely triggerable crash (SIGSEGV) at connection
setup → DoS. Info-leak potential is low: over-read bytes land in `resPQ.pq` / fingerprints,
which are only used for local nonce/fingerprint checks and are not echoed to the network.

## Fix

Convert the validated byte length to a prime count so the span matches the real data:

```cpp
return gsl::make_span(answer + 5, answerLen / sizeof(mtpPrime));
```

Legit traffic is unchanged (`answerLen` is always a multiple of 4 == `(len-5)*4`, so
`answerLen/4 == len-5`, the full body). Sub-prime malformed lengths truncate harmlessly.

## Reproduction

`repro.cpp` faithfully models `parseNotSecureResponse` + the TL string reader.

```
c++ -std=c++17 -fsanitize=address -g repro.cpp -o repro
./repro buggy   # ASan: heap-buffer-overflow READ 1596 bytes, 0 bytes after 420-byte region
./repro fixed   # span trimmed to real body; reader rejects; no OOB
```
