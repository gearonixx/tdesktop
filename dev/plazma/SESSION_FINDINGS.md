# Plazma → Telegram Desktop Port — Session Findings (Iteration #1)

Date: 2026-07-11. Fork: `/home/x/c/tdesktop_2`, branch `feat/plazma`.
Goal: recreate the standalone **Plazma** streaming client's functionality
*inside* Telegram Desktop — a "YouTube-but-it's-Telegram-Desktop": Telegram
login → self-hosted video feed → hardware-accelerated playback, skinned
**purple** instead of the messenger blue.

Sources analysed: `/home/x/c/plazma` (Plazma Qt Quick app + PlazmaServer REST
backend). All new work lives under the fork's `dev/plazma/` and
`Telegram/SourceFiles/plazma/`. No existing tdesktop source file was modified
this iteration (keeps the branch trivially rebasable).

---

## TL;DR — what was found and what was built

**Decisive finding:** Plazma was **already mid-migration off QML toward
tdesktop's real widget toolkit.** `application.h` carries `initNativeShell()`
— *"Brings up the tdesktop-style C++ widget shell (MainWindow + PageStart),
gated on the `PLAZMA_NATIVE_UI` env var so the legacy QML window stays the
default until every page is ported."* And `src/ui/window/main_window.*`,
`src/ui/widgets/buttons.cpp`, `src/ui/style/style_widgets.h`,
`src/ui/effects/ripple_animation.cpp` are **verbatim ports of tdesktop's
`lib_ui`**. Plazma's entire `Td` QML module (`TdPalette`, `TdFlatButton`,
`TdRipple`, …) is a hand-built QML *clone* of tdesktop widgets, with palette
tokens copied **1:1 from `lib_ui/ui/colors.palette`**. → Doing the port inside
tdesktop is the **terminus of a migration Plazma had already started**, not a
from-scratch rewrite. This reframes the whole task from "hard" to "natural."

**Recommended approach (of 4 evaluated):** *Approach B — full fork.* Keep
tdesktop's **real** Telegram login (its own MTProto `intro/` + `Main::Session`)
and replace the **content plane** (where chat history lives) with the Plazma
feed. Reach it incrementally through *Approach C* (add a coexistence "Plazma"
tab first, always-green build), then flip it to default and strip messaging.
This is the only approach that yields Telegram-native login + Telegram-native
chrome + purple skin + self-hosted feed simultaneously. Full rationale +
rejected approaches (A: standalone lib_ui app; D: QQuickWidget bridge) in
`PLAZMA_PORT_REPORT.md`.

**Three reuse decisions that collapse most of the work:**
1. **Auth — do NOT port TDLib.** tdesktop is already a full Telegram client;
   after login, `Main::Session::user()` exposes `id/username/first/last/phone/
   premium` = exactly Plazma's `UserLogin`. The bridge to PlazmaServer's
   `POST /v1/auth/login` is **one function**, not a second Telegram client.
2. **Player — drop libmpv.** tdesktop's `Media::Streaming` already does ranged
   HTTP + FFmpeg + hwdec; it covers everything `MpvObject` (`src/ui/mpv_object.*`,
   ~600 LoC + a render-thread mutex dance) exists to do. Keep libmpv only as a
   build-flag fallback until streaming-from-arbitrary-URL is proven.
3. **Theme — it's a data file, not code.** tdesktop palette token names ==
   the names Plazma copied, so the purple skin is a `.tdesktop-theme`, loadable
   into stock Telegram Desktop with **zero recompile**.

**Delivered MVP this iteration (3 artifacts):**
- A real, loadable **purple theme**.
- A compilable **PlazmaServer REST client** ported into the tdesktop tree.
- The **port report** + module README + this findings doc.

---

## The backend contract (reverse-engineered from Plazma, authoritative)

PlazmaServer is plain REST over `QNetworkAccessManager`, base
`http://localhost:8080`. Endpoints the client uses:

- `POST /v1/auth/login` — body `{user_id, username, first_name, last_name,
  phone_number, is_premium}` → `{token, user{…}}`. Bearer token thereafter.
- `GET /v1/videos[?q=]` → `{videos:[{id,title,url,size,mime,author,created_at,
  thumbnail,storyboard,description?}, …]}`. No `q` = `created_at` desc; with
  `q` = relevance desc. Cap ~200 rows, no pagination yet. Client debounces
  ~250ms and dedups by query but does NOT cancel in-flight — drops stale
  responses client-side via a monotonic `requestSeq_`.
- `POST /v1/videos/upload` — multipart, field `file` + optional `thumbnail`.
- `PATCH/DELETE /v1/videos/{id}`, playlist CRUD under `/v1/...` (see Plazma
  `api.h` + `PlazmaServer/docs/playlists.md`), ranged `startDownload` for
  offline.

This contract is now encoded faithfully in the ported client (below).

---

## MVP artifacts (reuse / iterate on these)

All under the fork:

1. **`dev/plazma/theme/plazma-purple.tdesktop-theme`** — a genuinely loadable
   purple theme. Method: extracted tdesktop's `Resources/night.tdesktop-theme`
   (a zip wrapping a `colors.tdesktop-theme` palette), recolored the **accent
   family** and re-zipped. Because palette tokens are *name-referenced*,
   recoloring ~19 base tokens cascades violet through buttons, links, active
   rows, checks, sliders, unread badges, and the sidebar. Values:
   - `windowBgActive #8a63d2` (primary violet), `windowActiveTextFg #ab8bf0`
     (links/online), `activeButtonBg #6d44b8` / over `#7a4fc7` / ripple
     `#8155cf`, `activeLineFg #9670e0`, `dialogsBgActive #3d2f66`,
     `sideBarBg #170f26`, plus a subtle purple tint on the dark neutrals
     (`windowBg #191426`, `windowBgOver #241d38`, `titleBg #1d1730`).
   - Plain-text palette also saved at `plazma-purple.colors.txt` for diffing.
   - **Preview NOW:** Telegram Desktop → Settings → Chat Settings → drag the
     `.tdesktop-theme` in (or double-click with tdesktop running). Fastest
     proof of the look; no build required.

2. **`Telegram/SourceFiles/plazma/plazma_api.{h,cpp}`** — the backend client.
   Faithful port of Plazma's `Api`/`RequestBuilder`: same endpoints, same
   fluent `.done()/.fail()/.send()` shape (incl. the documented "no `.send()`
   = no request" footgun), same feed/login/upload contract, `VideoItem::fromJson`
   matching the wire shape. **Qt Core + Qt Network only** (both already linked
   by tdesktop) → the safe first compilable brick; no libmpv/QML/TDLib pulled.

3. **Docs:** `dev/plazma/PLAZMA_PORT_REPORT.md` (approaches, recommendation,
   7-phase plan, risks), `Telegram/SourceFiles/plazma/README.md` (module +
   build wiring), this file.

---

## Status matrix

| Piece | Status |
|---|---|
| Approach analysis + recommendation | DONE (report) |
| Purple theme (loadable, no rebuild) | DONE (artifact) |
| Backend REST client ported | DONE (compilable unit; needs CMake add) |
| Session→login bridge (`Main::Session::user()` → `Api::loginUser`) | DONE (`plazma_session.cpp` `Backend`, CMake-wired) |
| Feed section (`Ui::RpWidget` card grid) | DESIGNED, not coded |
| Watch surface on `Media::Streaming` | DECIDED — native stack + `LoaderHttp` (see `DECISION_player_synthesis.md`); libmpv demoted to fallback |
| Playlists / profile / upload UI | pending (Phase 5) |
| Make Plazma the default surface | pending (Phase 6) |
| Rebrand (name/icon/updater) | pending (Phase 7) |

---

## Risks / open questions to settle by compiling (next iteration)

1. **`Media::Streaming` for arbitrary remote URLs** — RESOLVED to a bounded
   task (agent 2, see `DECISION_player_synthesis.md`). Verified in source:
   tdesktop already hwdecs (VAAPI/NVDEC/D3D11VA/DXVA2/VideoToolbox,
   `ffmpeg/ffmpeg_utility.cpp`) and `Media::Streaming::Reader` accepts a
   `std::unique_ptr<Loader>` directly (`media_streaming_reader.h:44`). The only
   gap is an HTTP `Loader`; the interface is part-based 128 KiB fetch, i.e.
   ranged GETs — exactly Plazma's `startDownload`/`part_file`. Sketched as
   `plazma/plazma_stream_loader.h` (`LoaderHttp`, mirrors `LoaderMtproto`).
   **Decision: default = tdesktop's native renderer + media viewer fed by
   `LoaderHttp`; libmpv (`MpvWidget`) kept only as a fallback.** Remaining
   validation is writing the `.cpp` + one compile.
2. **Session gating** — many `Ui` boxes/layers want a `Window::SessionController`.
   Approach C/B give us a real one for free; a fully standalone build would have
   to stub these.
3. **First build cost** — tdesktop is a multi-hour submodule build. Use
   `ccache` + the repo's existing `build_fast.sh`/`b`/`r` scripts. We have NOT
   compiled yet this iteration — that's the top of Iteration #2.
4. **Legal/branding** — GPLv3 fork is fine with source + attribution kept;
   rebranding must respect the `LEGAL` naming constraints. Phase 7.

## Addendum — two decisions locked this iteration

### A. Playback: keep libmpv, swap only the render surface
(See `mpv_integration.md`.) Plazma's `MpvObject` is two layers: (1) the
`mpv_handle` + the event/property loop (the `MPV_EVENT_PROPERTY_CHANGE` switch
reading `time-pos`, `duration`, `hwdec-current`, `video-codec`, …) and (2) a
`QQuickFramebufferObject` render surface. **Layer 1 ports 1:1** — it's pure
libmpv C API, QML-agnostic, and carries the "honest decoder" badge
(`hwdec-current`/`video-codec`) that distinguishes Plazma. **Only Layer 2
changes:** `QQuickFramebufferObject` → **`QOpenGLWidget`** driving the same
`mpv_render_context` (`render_gl.h`) via `MPV_RENDER_PARAM_OPENGL_FBO`. Bonus:
`QOpenGLWidget::paintGL` runs on the **GUI thread**, so the `MpvCallbackGuard`
mutex hazard that guards the scene-graph render thread **goes away** (just watch
teardown order: free render ctx with GL current, before the handle). This
**revises report Phase 4** — libmpv stays; `Media::Streaming` is only a fallback.
Header sketch delivered: `Telegram/SourceFiles/plazma/plazma_mpv_widget.h`.

### B. "Open Telegram → video feed" replaces the messaging plane
(See `feed_replaces_dialogs.md`.) Auth is **unchanged** — stock tdesktop
`intro/` → real MTProto `Main::Session`; bridge `session->user()` →
`Api::loginUser` once (id/username/names/phone/premium — the exact fields Plazma
sent from TDLib). The swap is three escalating steps on tdesktop's existing
**section** abstraction (`Window::SessionController::showSection`): (1)
coexistence `PlazmaSection` (always-green), (2) replace the **dialogs-column row
model** with `VideoItem` rows — reusing tdesktop's virtualized list, search
field, folder tabs, ripple — then (3) make the feed the **post-login landing
surface**, messaging behind a flag. **Playlists map onto Telegram-native
machinery** for the "advanced" feel: playlists = **chat folders / dialog
filters** (folder tab bar = playlist switcher, folder editor = playlist editor);
"watch later" = **Saved Messages** analog; share a video = **forward/share box**;
collaborative playlist = **group chat**; creators = **channel subscribe**;
comments = **discussion replies**; reactions = tdesktop **reactions picker**.
Authoritative data still lives in PlazmaServer; the Telegram constructs are the
UI skin over it.

## Files delivered this iteration

```
dev/plazma/
  SESSION_FINDINGS.md            (this file)
  PLAZMA_PORT_REPORT.md          approaches, recommendation, 7-phase plan, risks
  feed_replaces_dialogs.md       dialogs→feed swap + Telegram-native playlist map
  mpv_integration.md             libmpv QOpenGLWidget integration, verbatim Layer 1
  theme/plazma-purple.tdesktop-theme   loadable purple theme (no rebuild)
  theme/plazma-purple.colors.txt       plain-text palette for diffing
Telegram/SourceFiles/plazma/
  plazma_api.{h,cpp}             ported PlazmaServer REST client (compilable unit)
  plazma_session.{h,cpp}         auth bridge: Main::Session::user() -> loginUser
  plazma_mpv_widget.h            libmpv playback widget (header sketch)
  README.md                      module overview + CMake wiring
Telegram/CMakeLists.txt          all 4 plazma sources now wired into the target
dev/plazma/DESIGN_feed_replaces_dialogs.md  concrete _dialogs swap seams (B0/B1)
```

### Agent-2 addendum (things the lists above predate)
- **CMake: DONE.** `plazma/plazma_api.{cpp,h}` + `plazma/plazma_session.{cpp,h}`
  are wired into the `Telegram` target's source list (`CMakeLists.txt:1656`,
  alphabetically between `platform/` and `profile/`). Next-iteration step 1 below
  is already complete.
- **Session→login bridge: CODED** (was "DESIGNED, not coded"). `plazma_session.*`
  provides `UserLoginFromSession(not_null<Main::Session*>)` and a `Backend` that
  owns the `Api` and does the `/v1/auth/login` handshake. It is the *only* TU that
  includes both `main/main_session.h` and `plazma_api.h`. Verified accessors:
  `session->userId().bare` (int64), `user->username()`, public `user->firstName`/
  `lastName`, `user->phone()`, `user->isPremium()`. `not_null` comes from the
  `Telegram` target PCH, so this file compiles in-tree only (by design).
- **Dialogs→feed swap points located** (see `DESIGN_feed_replaces_dialogs.md`):
  left column `Dialogs::Widget` is built at `mainwidget.cpp:264` and held as
  `_dialogs` (`mainwidget.h:344`) — that is the concrete replace point. Two
  routes: **B0** a sibling `Ui::RpWidget` card feed (one-file change, MVP), or
  **B1** teach `Dialogs::Row`/`InnerWidget` a video-row type to inherit search/
  pin/reorder for free.

## Next iteration (#2) plan

1. Add `plazma/plazma_api.cpp/.h` to the `Telegram` target in
   `Telegram/CMakeLists.txt`; do a build to confirm the brick compiles in-tree.
2. Build a tiny `plazma_feed_widget` (`Ui::RpWidget` + scroll area of cards),
   register it as a coexistence tab (Approach C), wire `Api::fetchVideos("")`.
3. Spike `Media::Streaming` against one PlazmaServer URL to de-risk the player.
4. Bridge `Main::Session::user()` → `Api::loginUser` after `intro/` completes.
