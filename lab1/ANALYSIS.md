# Stack Buffer Overflow in rlottie `bufferToRle` — RCE via Animated Sticker

## Summary

A stack buffer overflow in `Telegram/ThirdParty/rlottie/src/vector/vrle.cpp`
allows an attacker to overwrite 2048 bytes past a 256-element stack array with
attacker-influenced data (VRle::Span fields derived from Lottie path geometry).

Delivery: a `.tgs` animated sticker triggers the overflow on first render, zero
interaction after message receipt on clients with sticker auto-play (the default).

---

## Root Cause

### `bufferToRle` has no output-capacity parameter

```cpp
// vrle.cpp:577 — no outCapacity parameter
size_t bufferToRle(uchar *buffer, int bufferLen, int size,
                   int offsetX, int y, VRle::Span *out)
{
    // writes to *out++ with NO capacity check
    ...
    out->y        = y;
    out->x        = offsetX + curIndex;
    out->len      = i - curIndex;
    out->coverage = value;
    out++;              // ← potential write past end of caller's buffer
    count++;
```

### Both callers allocate 256 spans but allow up to 512

**`rleOpGeneric` (vrle.cpp:616):**
```cpp
std::array<VRle::Span, 256> temp;       // 2048 bytes on stack
...
size_t size = bufferToRle(
    array.data(), kBufferSize,           // kBufferSize = 1024
    std::max(aLength, bLength),          // can be up to 1024
    offset, y,
    temp.data());                        // NO capacity passed or checked
if (available >= size) {                 // ← guard is AFTER the overflow
```

**`rleSubstractWithRle` (vrle.cpp:681):** identical pattern.

### Worst-case span count

`bufferToRle` emits one span per contiguous run of non-zero coverage bytes.
With a 1024-byte coverage buffer in alternating pattern (0, 255, 0, 255, …):

    spans = 1024 / 2 = 512

512 > 256 → **256 spans × 8 bytes = 2048 bytes overflow** into adjacent stack data.

---

## Trigger Path

```
Telegram receives .tgs sticker (gzip Lottie JSON ≤ 64 KB, passes server checks)
 │
 └─ rlottie parse:  masksProperties → parseMaskProperty()          [lottieparser.cpp:1019]
                    mode: "f" → LOTMaskData::Mode::Difference
 │
 └─ LOTLayerMaskItem::maskRle()                                    [lottieitem.cpp:368]
      rle = rle + mask_0_rle   (mode "a" / Add)
      rle = rle ^ mask_1_rle   (mode "f" / Difference)            [lottieitem.cpp:390]
 │
 └─ VRle::operator^                                                [vrle.h:148]
      → VRleData::opGeneric(a, b, OpCode::Xor)
      → rleOpGeneric(&aObj, &bObj, &tresult, Operation::Xor)       [vrle.cpp:309]
 │
 └─ rleOpGeneric:                                                  [vrle.cpp:613]
      std::array<VRle::Span, 256> temp;       // 256-slot stack buffer
      blit(aStart, …, array.data(), 1024, …)  // fills array with 255
      blitXor(bStart, …, array.data(), 1024, …) // alternates 0/255
      bufferToRle(array.data(), 1024, 1024, 0, y, temp.data())
                                               // emits 512 spans → OVERFLOW
```

---

## PoC Sticker Design

### Composition layout
| Field | Value | Reason |
|-------|-------|--------|
| Canvas width | 1024 px | Needed to produce > 512-px coverage buffer |
| Canvas height | 128 px | Arbitrary |
| Layer type | Shape (ty=4) | Must have renderable content to trigger mask ops |

### Mask 0 — mode `"a"` (Add)
A single solid rectangle covering the full 1024×128 canvas.

Since the initial `rle` is empty, `operator+` short-circuits:
```cpp
if (empty()) return o;   // returns mask_0_rle directly, no opGeneric call
```
Result: `rle` = one span per row, x=0 len=1024, coverage=255.

### Mask 1 — mode `"f"` (Difference / XOR)
A single closed polygon tracing a **comb** with 512 one-pixel-wide teeth:

```
(0,0)→(1,0)→(1,128)→(2,128)→(2,0)→(3,0)→(3,128)→(4,128)→(4,0)→ … close
```

When rasterised with non-zero winding rule, alternate columns are filled:
filled at x=0, x=2, x=4, …, x=1022 → **512 spans per scanline, coverage=255**.

### What happens inside `rleOpGeneric` (per scanline)

```
blit(mask_0_spans)   → array[0..1023] = 255  (all pixels covered)
blitXor(comb_spans)  → array[0]=0, array[2]=0, array[4]=0, ...
                     → array = [0, 255, 0, 255, ...] × 512 pairs
bufferToRle(size=1024) → 512 spans into temp[256]  → OVERFLOW by 256×8=2048 bytes
```

### Content of overflowed data
Each `VRle::Span` written beyond `temp[255]` contains:
- `x`:  (short) position derived from path geometry
- `y`:  (short) current scanline
- `len`: (ushort) run length from path geometry
- `coverage`: (uchar) anti-aliased coverage value

The attacker controls the path geometry → controls the exact bytes written
beyond the stack buffer → controlled stack corruption.

---

## Confirmed with AddressSanitizer

```
ERROR: AddressSanitizer: stack-buffer-overflow on address … at pc … in bufferToRle
WRITE of size 2 at offset 5666 in frame main
  [3616, 5664) 'temp_VULNERABLE'   ← 256-span (2048-byte) stack buffer
Memory access at offset 5666 overflows this variable
```

See `lab1/repro.cpp` (standalone) and `lab1/repro_asan` (pre-built).
Run `./repro_asan` for the immediate ASan crash.

---

## Impact

- **Platform:** All platforms using this rlottie version (Telegram Desktop, Android, iOS).
- **Interaction:** Zero-click after message receipt; sticker auto-plays by default.
- **Overflow content:** Attacker-controlled VRle::Span data (geometry from path JSON).
- **Stack corruption:** Overwrites 2048 bytes past `temp[]`, hitting local variables,
  saved RBP, and return address.
- **Desktop:** No sandbox; direct RCE if ASLR/CFI bypassed.
- **Mobile:** RCE in sandbox, sandbox escape required for full compromise.

---

## Fix

Option A — expand `temp` to the true worst-case size:
```cpp
// rleOpGeneric, line 616  (and same in rleSubstractWithRle, line 681)
std::array<VRle::Span, 512> temp;   // kBufferSize/2 is the maximum span count
```

Option B — pass output capacity into `bufferToRle` and enforce it:
```cpp
size_t bufferToRle(uchar *buffer, int bufferLen, int size,
                   int offsetX, int y, VRle::Span *out, size_t outCapacity)
{
    ...
    if (value) {
        if (count >= outCapacity) break;   // ← new guard
        out->...;
        out++;
        count++;
    }
```

Both callers must be updated consistently.

---

## v2: Bypassing the 512×512 Constraint

The original PoC (poc.tgs) used `w:1024` which Telegram rejects (stickers must be 512×512).

**Solution: Layer scale transform.**

The Lottie layer's `ks.s` (scale) property is applied to mask paths BEFORE rasterization
(lottieitem.cpp:206: `mFinalPath.transform(parentMatrix)`), and the rasterizer receives
NO clip rect (line 208). A 2x scale on a 512×512 canvas produces mask spans up to 1024px
wide — exactly hitting `kBufferSize=1024` and producing >256 spans.

### End-to-end ASan confirmation (poc_v2.json, 512×512 + 2x scale):

```
ERROR: AddressSanitizer: stack-buffer-overflow on address ... in bufferToRle
WRITE of size 2 at offset 3266 in frame rleOpGeneric (vrle.cpp:616)
  [1216, 3264) 'temp' (line 617) <== Memory access at offset 3266 overflows this variable

Call stack:
  bufferToRle          (vrle.cpp:592)
  rleOpGeneric         (vrle.cpp:653)
  VRle::operator^      (vrle.h:154)
  LOTLayerMaskItem::maskRle (lottieitem.cpp:390)
  LOTLayerItem::render (lottieitem.cpp:310)
  LOTCompItem::render  (lottieitem.cpp:185)
  rlottie::Animation::renderSync (lottieanimation.cpp:321)
```

### Why the transform bypass works

| Property | Original PoC | v2 PoC |
|----------|-------------|--------|
| Canvas | 1024×128 | **512×512** |
| Layer scale | 100% | **200%** |
| Effective span width | 1024px | 1024px |
| Telegram validation | REJECTED | **PASSES** |
| .tgs size | 5 KB | **2.7 KB** |
| ASan crash | Yes (standalone only) | **Yes (full rlottie render)** |

Telegram validates the composition's declared `w`/`h` (must be 512×512) but does NOT
validate that layer transforms keep content within canvas bounds. The mask rasterizer
has no clipping. This makes the overflow **deliverable through Telegram**.

---

## Files in this lab

| File | Purpose |
|------|---------|
| `repro.cpp` | Standalone C++ reproducer — extracts the exact vulnerable logic, confirms the overflow with and without ASan |
| `repro_asan` | Pre-built ASan binary (run `./repro_asan` to see the crash) |
| `gen_poc.py` | Original PoC generator (w=1024, does NOT work on Telegram) |
| `gen_poc_v2.py` | **v2 PoC generator (512×512 + 2x scale, WORKS on Telegram)** |
| `poc.tgs` | Original .tgs sticker (1024px, rejected by Telegram) |
| `poc.json` | Original uncompressed JSON |
| `poc_v2.tgs` | **v2 .tgs sticker (512×512, passes Telegram validation, triggers overflow)** |
| `poc_v2.json` | v2 uncompressed JSON for ASan testing |
| `render_test.cpp` | Skeleton for end-to-end test against the rlottie render API |
| `Makefile` | Build all targets: `make all`, `make run_asan` |
