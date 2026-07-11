# Plazma module (port scaffold)

Ported streaming functionality of the standalone **Plazma** Qt Quick app into
Telegram Desktop. Goal: same feature set (Telegram-account login → self-hosted
video feed → hardware-accelerated playback), but rendered with tdesktop's own
`Ui` widget toolkit and themed purple, instead of a separate QML app.

## What's here now (MVP brick 1)

- `plazma_api.{h,cpp}` — self-hosted-backend REST client. A faithful port of
  Plazma's `src/api.{h,cpp}`: same endpoints, same `RequestBuilder` fluent
  shape, same `/v1/videos` feed contract. Depends only on Qt Core + Qt Network
  (already linked by tdesktop), so it compiles as a standalone unit — the safe
  first integration step.

## Not here yet (next bricks — see the report)

- `plazma_feed_widget.*` — an `Ui::RpWidget` grid of video cards.
- `plazma_watch_widget.*` — playback surface reusing `Media::Streaming`
  (tdesktop's FFmpeg player) instead of libmpv.
- `plazma_section.*` — a `Window::SectionWidget` so the feed lives where the
  chat history normally does.
- Session bridge — feed `Main::Session::user()` identity into `Api::loginUser`.

## Build wiring

Add the sources to `Telegram/lib_tgvoip`-style target lists — concretely in
`Telegram/CMakeLists.txt` under the `Telegram` target `sources` list:

```cmake
    plazma/plazma_api.cpp
    plazma/plazma_api.h
```

## Purple theme

`dev/plazma/theme/plazma-purple.tdesktop-theme` is a ready, loadable theme
(Settings → Chat Settings → drag the file in, or double-click). No rebuild
needed to preview the look — that's the fastest MVP demo.

See `dev/plazma/PLAZMA_PORT_REPORT.md` for the full plan.
