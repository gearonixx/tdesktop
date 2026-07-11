# Porting Plazma into Telegram Desktop — Approaches, Recommendation, MVP

**Date:** 2026-07-11
**Fork:** `/home/x/c/tdesktop_2`, branch `feature/plazma-mvp`
**Source project analysed:** `/home/x/c/plazma` (Plazma Qt Quick app + PlazmaServer)

---

## 1. What Plazma actually is

A native Qt **Quick** (QML) desktop app, C++20 engine, that:

1. **Authenticates through a Telegram account** via **TDLib** — the standard
   Telegram login (phone → code → 2FA), then POSTs the resulting identity
   (`user_id, username, first/last name, phone, is_premium`) to a self-hosted
   backend (`POST /v1/auth/login`) which returns a bearer token.
2. **Shows a video feed** from **PlazmaServer** (`GET /v1/videos[?q=]`), plus
   playlists, profile, upload (`POST /v1/videos/upload`, multipart), rename/
   delete, and offline downloads (ranged streaming GET).
3. **Plays video** through **libmpv** wrapped as a `QQuickFramebufferObject`
   (`MpvObject`), with hwdec (VA-API/NVDEC) and an honest decoder readout.
4. Renders everything through a **hand-built QML clone of tdesktop's own UI
   toolkit** — the `Td` module (`TdPalette`, `TdFlatButton`, `TdRipple`,
   `TdPopupMenu`, `TdScrollArea`, …). The palette tokens are copied **1:1**
   from `lib_ui/ui/colors.palette`.

### The decisive discovery

Plazma was **already mid-migration away from QML toward tdesktop's real widget
toolkit**. Evidence in the repo:

- `application.h` has `initNativeShell()` — *"Brings up the tdesktop-style C++
  widget shell (MainWindow + PageStart), gated on the `PLAZMA_NATIVE_UI` env
  var so the legacy QML window stays the default until every page is ported."*
- `src/ui/window/main_window.{h,cpp}`, `src/ui/widgets/buttons.cpp`,
  `src/ui/style/style_widgets.h`, `src/ui/effects/ripple_animation.cpp` are
  **verbatim ports of `lib_ui`** (`Ui::RpWidget`, ripple animations, the style
  system).

So the project's own trajectory was: *stop reimplementing tdesktop's look in
QML; adopt tdesktop's actual widgets.* Doing the port **inside** tdesktop is
the natural terminus of that trajectory, not a rewrite from scratch.

---

## 2. The core architectural mismatch

| Concern | Plazma | Telegram Desktop |
|---|---|---|
| UI framework | Qt **Quick**/QML | Qt **Widgets** + custom `Ui::RpWidget` |
| Telegram auth | **TDLib** (separate client) | **own MTProto** stack (`mtproto/`, `intro/`, `Main::Session`) |
| Video playback | **libmpv** (`MpvObject`) | own **FFmpeg** pipeline (`Media::Streaming`, `Media::Player`) |
| App shell | `QStackedWidget` of pages | `Window::Controller` → `MainWindow` → sections |
| Backend | PlazmaServer REST (`QNetworkAccessManager`) | MTProto to Telegram DC; QNAM only for side jobs |

The tension: tdesktop's entire shell (`Window::SessionController`, dialogs,
history) is built around **an authenticated MTProto session to Telegram**.
A streaming app wants Telegram *identity* but a *self-hosted* content backend.
Every approach below is really a different answer to *"how much of tdesktop's
session machinery do we keep?"*

---

## 3. Approaches considered

### Approach A — Standalone port using tdesktop's `lib_ui` only (no MTProto)

Take only tdesktop's **UI libraries** (`lib_ui`, `lib_base`, `lib_ffmpeg`,
`codegen`, the style system) and build a *new small app* around them — exactly
what `initNativeShell()` was starting. Keep TDLib for auth, keep libmpv or
swap to `lib_ffmpeg`.

- **Pros:** clean; no fight with the MTProto session model; smallest binary;
  Plazma stays Plazma, just with real tdesktop widgets. Finishes the migration
  that was already underway.
- **Cons:** you don't get tdesktop's *chrome* for free (settings framework,
  layer/box system, media viewer) unless you also pull those libs, and several
  are entangled with `Main::Session`. This is "Plazma with tdesktop widgets",
  not "Telegram Desktop turned into a streaming app".

### Approach B — Full fork: repurpose tdesktop's shell, replace content (RECOMMENDED)

Branch tdesktop, **keep** the real Telegram login (`intro/`) and
`Main::Session`, and **replace the content plane** — where chat history lives —
with a Plazma streaming section. The user logs into Telegram *for real* (tdesktop's
own MTProto), and the identity is forwarded to PlazmaServer's `/v1/auth/login`.

- **Pros:** matches the user's stated intent ("instead of messaging it's a
  streaming app with the same UI"). Reuses the *real* login (no TDLib, no second
  Telegram client), the real theme engine (→ purple in minutes), the real
  FFmpeg player, boxes/layers, settings, tray, updater. Maximum reuse.
- **Cons:** biggest surface to understand; must carve out the messaging UI
  (dialogs list, history, compose) and graft the feed in its place; the DC
  connection stays live even though we don't use it for content.
- **Why recommended:** it is the only approach that yields *"YouTube-but-it's
  Telegram Desktop"* — Telegram-native login + Telegram-native chrome + purple
  skin + self-hosted feed. It also reuses `Media::Streaming` so libmpv can be
  dropped entirely.

### Approach C — Coexistence: add a "Plazma" tab alongside chats

Leave messaging intact; add Plazma as a new **top section** reachable from the
left rail (like Settings or the calls list). Same shell, new destination.

- **Pros:** lowest risk, incremental, always-buildable; good migration *path*
  toward B (build the section here, then make it the default).
- **Cons:** it's a plugin, not "the app is now a streaming client". Doesn't
  satisfy the "instead of messaging" framing on its own.

### Approach D — Rewrite tdesktop's UI in QML / QML-in-Widgets bridge

Embed the existing Plazma QML via `QQuickWidget` inside a tdesktop window.

- **Rejected:** worst of both worlds — two UI frameworks, two event loops, the
  purple/theme story splits in half, and it throws away the migration Plazma
  had already started *away* from QML. Only useful as a throwaway smoke test.

---

## 4. Recommended path: **B, reached incrementally via C**

Ship the coexistence section first (always-green build), then flip it to the
default surface and strip messaging. Concretely:

```
Phase 0  Purple theme                 [DONE — dev/plazma/theme/*.tdesktop-theme]
Phase 1  Backend client               [DONE — SourceFiles/plazma/plazma_api.*]
Phase 2  Feed section (coexist tab)    Ui::RpWidget grid of video cards
Phase 3  Session bridge               Main::Session::user() -> Api::loginUser
Phase 4  Watch surface                Media::Streaming (drop libmpv)
Phase 5  Playlists / profile / upload port remaining pages
Phase 6  Make Plazma the default; hide messaging behind a flag
Phase 7  Rebrand (app name/icon/updater feed)
```

### Key reuse decisions

- **Auth:** *Do not port TDLib.* tdesktop is already a full Telegram client;
  after `intro/` completes, `Main::Session::user()` exposes id / username /
  names / phone / premium — exactly Plazma's `UserLogin`. Bridge = one function.
- **Player:** *Drop libmpv.* Use `Media::Streaming::Player` +
  `Media::Streaming::Document`-style loader pointed at the PlazmaServer URL.
  tdesktop already does ranged HTTP streaming + FFmpeg hwdec; `MpvObject`'s
  entire job is already covered. (Fallback: keep libmpv behind a build flag if
  a specific hwdec path is missing.)
- **Networking:** PlazmaServer is plain REST, so a dedicated
  `QNetworkAccessManager` (the ported `Plazma::Api`) is correct — do **not**
  try to tunnel it through MTProto.
- **Theme:** tdesktop's palette tokens are the same names Plazma copied, so the
  purple skin is a `.tdesktop-theme` file (Phase 0, done) — no recompile to
  preview.

---

## 5. MVP delivered in this session

1. **`dev/plazma/theme/plazma-purple.tdesktop-theme`** — a real, loadable
   purple theme. Derived from tdesktop's `night.tdesktop-theme` by recoloring
   the accent family (violet `#8a63d2` primary, `#6d44b8` buttons) plus a subtle
   purple tint on the dark backgrounds. Because tdesktop palette tokens are
   name-referenced, recoloring the base accent cascades through buttons, links,
   active rows, checks, sliders, unread badges, sidebar. **Loadable today into
   stock Telegram Desktop** (Settings → Chat Settings → drag file in) — the
   fastest possible proof of the look. Plain-text palette also at
   `dev/plazma/theme/plazma-purple.colors.txt`.

2. **`Telegram/SourceFiles/plazma/plazma_api.{h,cpp}`** — the backend client,
   a faithful port of Plazma's `Api`: same endpoints, same fluent
   `RequestBuilder`, same `/v1/videos` + `/v1/auth/login` + upload contract.
   Qt Core + Qt Network only, so it's the safe first compilable brick.

3. **This report** + `README.md` in the module folder with the wiring points.

### To preview the theme right now
Open Telegram Desktop → Settings → Chat Settings → *Create/Choose theme* →
drag `plazma-purple.tdesktop-theme` in. (Or double-click the file with tdesktop
running.)

---

## 6. Concrete next steps (Phase 2)

1. Add `plazma/plazma_api.cpp/.h` to the `Telegram` target in
   `Telegram/CMakeLists.txt`.
2. Create `plazma/plazma_feed_widget.{h,cpp}` — an `Ui::RpWidget` owning a
   scroll area of video cards. Card = thumbnail (`Ui::RpWidget` painting a
   rounded pixmap loaded from `VideoItem::thumbnail`) + title + author. Reuse
   `Ui::FlatLabel`, `Ui::RoundButton`, `Ui::RippleAnimation` (all already in
   `lib_ui`, and already partially copied into Plazma — cross-check names).
3. Wire `Api::fetchVideos("")` → repopulate cards. Add the search field
   (`Ui::InputField`) with the same debounce Plazma uses (~250ms).
4. Register the section as a coexistence destination (Approach C) so it builds
   and runs without touching the messaging UI yet.

## 7. Risks & unknowns to validate by compiling

- **`Media::Streaming` for arbitrary HTTP URLs:** confirm it accepts a plain
  remote URL loader (it's built around Telegram documents/local files). May
  need a small `Streaming::Loader` that wraps `Api::startDownload`-style ranged
  GETs. This is the biggest technical unknown; keep libmpv as a fallback until
  proven.
- **Session gating:** many `Ui` boxes/layers assume a `Window::SessionController`.
  The coexistence approach gives us a real one for free; a fully standalone
  build (Approach A) would have to stub these.
- **Build time:** tdesktop is a multi-hour first build with submodules. Budget
  for it; use `ccache` and the repo's existing `build_fast.sh`.
- **Legal/branding:** repurposing tdesktop is fine under GPLv3 provided source
  and attribution are kept; rebranding must follow the `LEGAL` file's naming
  constraints. Handle in Phase 7.
