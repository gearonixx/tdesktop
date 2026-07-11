# Decision: combine Plazma + Telegram's native video stack (the player synthesis)

Iteration #1, agent 2. Supersedes the earlier "keep libmpv" lean in
`mpv_integration.md` **for the default path**, now that tdesktop's own video
stack has been read in detail. Grounded in the actual source; file/line refs are
under `Telegram/SourceFiles`.

## What tdesktop already gives us (verified in source)

tdesktop is **not** a software-only inline player — it is a full hardware
pipeline plus a polished media viewer:

- **Hardware decode on every platform** — `ffmpeg/ffmpeg_utility.cpp` sets up
  `av_hwdevice_ctx_create` for **VAAPI** (`:183`), **CUDA/NVDEC** (`:163`),
  **D3D11VA/DXVA2** (`:250-251`), **VideoToolbox** (`:254`). This is exactly the
  hwdec matrix libmpv gave Plazma. → libmpv's headline advantage is *already in
  the box*.
- **A decode/render pipeline you can point anywhere** — `Media::Streaming::Reader`
  takes a `std::unique_ptr<Loader>` **directly** (`media/streaming/media_streaming_reader.h:44`).
  A `Player` wraps a `Reader`. The renderer is tdesktop's own — it composites
  natively into the Qt Widgets scene (rounded window, layers, top bar) with no
  foreign GL surface.
- **The media viewer** — `media/view/media_view_overlay_widget.*` (fullscreen,
  controls, gestures, PiP, system media keys) is already wired to `Media::Streaming`.
- **Disk caching, speed estimation, priority-based part fetching** — for free in
  the streaming layer.

## The one missing piece — and it's small and well-specified

`Media::Streaming` has exactly two `Loader`s: `LoaderLocal` (a `QIODevice`) and
`LoaderMtproto` (Telegram DC). **There is no HTTP loader.** But the `Loader`
interface (`media_streaming_loader.h`) is tiny and part-based:

- `static constexpr kPartSize = 128 * 1024;`
- `void load(int64 offset)` → fetch the 128 KiB part at `offset`
- `rpl::producer<LoadedPart> parts()` → emit `{offset, bytes}` back
- `size()`, `baseCacheKey()`, priority/cancel/stop bookkeeping

**`load(offset)` is literally a ranged HTTP GET** (`Range: bytes=offset-offset+kPartSize-1`)
— which is *exactly* what Plazma's `Api::startDownload(url, resumeOffset)` +
`storage/part_file.*` already do (ranged fetch, resume, 206/200 handling). So the
"biggest unknown" from the earlier report is really a **bounded ~250-line class**,
modeled on `LoaderMtproto` (same `PriorityQueue _requested` + `rpl::event_stream
<LoadedPart> _parts`, but `MTP::Sender` → `QNetworkAccessManager`).

## The decision

**Default player = tdesktop's native `Media::Streaming` + renderer + media
viewer, fed by a new `Media::Streaming::LoaderHttp` that streams PlazmaServer
URLs.** This is the literal "combine the best + Telegram's renderer":

| Take from… | What |
|---|---|
| **Telegram (tdesktop)** | the renderer (native compositing), hwaccel (VAAPI/NVDEC/D3D11/VT), the media-viewer overlay + controls, disk cache, speed/priority streaming — no new dependency (FFmpeg already linked) |
| **Plazma** | the *content source*: the PlazmaServer feed + the ranged-GET/resume mechanics (`startDownload` + `part_file`) that become `LoaderHttp`'s internals; the "honest decoder" badge idea (tdesktop knows its chosen hwaccel type too — surface it on an overlay label) |
| **Bridge** | `LoaderHttp : Media::Streaming::Loader` — the ~250 lines that join them |

### Why this beats the pure-libmpv path (revises `mpv_integration.md`)

- **Native rendering** — video composites inside tdesktop's own scene (rounded
  corners, layers, the real media viewer), instead of a foreign `QOpenGLWidget`
  child that has to be made to fit.
- **The media viewer for free** — fullscreen, PiP, scrubbing, system media keys,
  gestures — thousands of lines of UX we'd otherwise re-skin.
- **No second dependency / no duplicate FFmpeg** — libmpv would ship its own.
- **No capability lost** — tdesktop already hwdecs, so the decoder badge survives.

### Where libmpv still earns a place (keep, don't lead)

Keep `Plazma::MpvWidget` (the `QOpenGLWidget` from `plazma_mpv_widget.h`) behind
a build/runtime flag as a **fallback**, for: platforms where a codec/hwaccel path
is weak in tdesktop's FFmpeg build, or if we want mpv's richer subtitle/filter/
track handling on the watch page. Default stays native; mpv is the escape hatch.

## Concrete build order for the player

1. `plazma/plazma_stream_loader.{h,cpp}` — `LoaderHttp : Media::Streaming::Loader`.
   Internals lifted from Plazma `startDownload`/`part_file`: a `QNetworkAccessManager`
   issuing `Range` GETs per `load(offset)`, feeding `_parts`. `size()` from a
   HEAD (or the first response's `Content-Range`). `baseCacheKey()` synthesized
   from a hash of the URL. (Header sketch delivered alongside this doc.)
2. Build a `Reader(std::make_unique<LoaderHttp>(url, size), …)` → `Player`, driven
   from the Plazma watch section. Point it at `VideoItem::url`.
3. Feed inline autoplay in the card list through the *same* loader → same Player,
   small size, muted — like Telegram's animated-thumb autoplay.
4. Open fullscreen through the existing `Media::View::OverlayWidget` path so the
   whole viewer UX comes along.
5. Surface the chosen hwaccel type on an overlay `Ui::FlatLabel` = the "honest
   decoder" badge, now reading tdesktop's FFmpeg hwaccel instead of mpv's.

## Net effect on the plan

- The report's Phase-4 "biggest technical unknown" is **downgraded to a bounded
  task** (`LoaderHttp`), because the `Loader` contract is part-based ranged fetch
  and Plazma already implements ranged fetch.
- libmpv moves from "the player" to "the fallback."
- Everything else (feed = dialogs swap, playlists = folders, purple theme, auth
  bridge) is unchanged.
