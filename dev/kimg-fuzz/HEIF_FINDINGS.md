# HEIF decoder fuzzing — findings

Target: Telegram Desktop's bundled image plugins (`kimageformats`) driven the
same way Qt drives them from `QImageReader` / `App::readImage`. Harness
`harness.cpp` feeds attacker bytes into `QOIHandler`, `QAVIFHandler`,
`HEIFHandler`, `QJpegXLHandler` `::read()`. Build: libFuzzer + ASan (`fuzz_heif_cov`).

Campaign: fork-mode, `-rss_limit_mb=4096`, ran ~08:19–10:1x on 2 cores.

## Summary

| ID | Severity | Class | Component | Reproducers | Status |
|----|----------|-------|-----------|-------------|--------|
| **HEIF-1** | **Medium** (DoS) | null/wild-pointer dereference (SEGV read) | **libheif 1.22** via tdesktop HEIF plugin | **77** (all identical) | ASan-reproduced |

**77 crash artifacts triaged → 1 unique bug.** No OOB-write / UAF / heap
corruption surfaced in this run (all 77 collapse to HEIF-1). The other three
formats (QOI/AVIF/JXL) produced no crashes here — but note HEIF-1 fires on most
mutated inputs, so it likely *masks* those formats; they deserve a follow-up run
with HEIF-1 patched or the HEIF handler stubbed.

---

## HEIF-1 — SEGV (read @ 0x0d) in `heif_context_get_primary_image_handle`

**Deterministic null/wild-pointer dereference.** Every one of the 77 inputs
faults at the *same* address, `0x000000000000000d` (zero page, READ) — so this
is a structural bug, not attacker-controlled data.

### Crash
```
AddressSanitizer: SEGV on unknown address 0x00000000000d  (READ)  Hint: zero page
  #0 libstdc++ __dynamic_cast
  #2 std::dynamic_pointer_cast<ImageItem_Error, ImageItem>(...)      shared_ptr.h:720
  #3 heif_context_get_primary_image_handle    libheif-1222/.../heif_context.cc:169:23
  #4 HEIFHandler::ensureDecoder()             kimageformats/src/imageformats/heif.cpp:463
  #5 HEIFHandler::ensureParsed() const        heif.cpp:429
  #6 HEIFHandler::read(QImage*)               heif.cpp:89
```

### Mechanism
At `heif_context.cc:169`, after the `if (!primary_image)` null check (line 163)
passes, the code does `std::dynamic_pointer_cast<ImageItem_Error>(primary_image)`.
For these malformed inputs `primary_image` is **non-null but garbage** — its
stored pointer is ~`0x0d`, so it survives the `!primary_image` check but faults
when `__dynamic_cast` reads the object's vtable. In other words libheif's parse
of a malformed primary item yields a `shared_ptr<ImageItem>` holding an invalid
pointer that the null-guard doesn't catch. The constant `0x0d` across all 77
inputs confirms a fixed structural defect (partially-constructed / mis-typed
primary item), not a data-dependent overflow.

### Impact / reachability
- **Client-side crash (DoS).** Telegram Desktop decodes images via
  `kimageformats`; a HEIF image/document from an attacker, decoded for
  preview/thumbnail, crashes the client. Repeatable and deterministic.
- **Not confirmed RCE.** The fault is a *read* of a fixed near-null address
  (`0x0d`); there is no evidence of an attacker-controlled write or of a
  data-dependent pointer here. Classifying as **Medium / DoS**, not overstating.
  (A deeper look at whether `0x0d` can be steered by input would be needed to
  rule RCE in or out, but the constant address argues against attacker control.)
- **Bug lives in libheif 1.22 (third-party)**, consumed by tdesktop. Fix is
  upstream libheif (or bump the vendored version), plus a defensive check in
  `HEIFHandler::ensureDecoder()` for a failed/degenerate primary handle.

### Reproduce
```
cd /home/x/c/tdesktop/dev/kimg-fuzz
ASAN_OPTIONS=abort_on_error=1 ./fuzz_heif_cov artifacts_heif_cov/crash-2d659f7a6823de748b59fac98f9167aad9cfa217
```
- Smallest reproducer: **67 bytes** (`crash-2d659f7a…`); range 67–845 bytes.
- All 77 artifacts under `artifacts_heif_cov/` reproduce HEIF-1.

### Suggested fix
1. In `heif_context.cc:169`, guard the cast: verify `primary_image` is a fully
   constructed item (validity flag / non-degenerate pointer) before the
   `dynamic_pointer_cast`, and return `heif_error_Invalid_input` otherwise.
2. Defensively, in `HEIFHandler::ensureDecoder()` (heif.cpp:463) treat a
   non-`heif_error_Ok` return (or a null `handle`) as a decode failure instead
   of proceeding.
3. Add these 77 inputs (deduped to 1) as a regression seed.
