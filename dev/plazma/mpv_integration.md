# Integrating Plazma's libmpv player into Telegram Desktop

## The one thing that changes, and the one that doesn't

`MpvObject` in Plazma is two things bolted together:

1. **A control/event layer** — the `mpv_handle`, `mpv_set_wakeup_callback`, the
   `handleMpvEvents()` loop you pasted (the `MPV_EVENT_FILE_LOADED` /
   `MPV_EVENT_END_FILE` / `MPV_EVENT_PROPERTY_CHANGE` switch that reads `pause`,
   `duration`, `time-pos`, `demuxer-cache-time`, `volume`, `mute`, `speed`,
   `width/height`, `core-idle`, `eof-reached`, **`hwdec-current`**,
   **`video-codec`**), plus `mpv_command`/`mpv_set_property` for load/seek/etc.
2. **A render surface** — `QQuickFramebufferObject` + an `MpvRenderer` that owns
   an `mpv_render_context` and blits into the Qt **scene-graph** FBO.

**Layer 1 ports to tdesktop 1:1 — copy it verbatim.** It's pure libmpv C API;
it has nothing to do with QML. Every property observer, including the
`hwdec-current` / `video-codec` readout that gives Plazma its "honest decoder in
use" badge, moves over unchanged.

**Only Layer 2 changes.** tdesktop is Qt **Widgets**, not Qt Quick — there is no
scene graph to render into. So `QQuickFramebufferObject` becomes a
**`QOpenGLWidget`** that drives the *same* `mpv_render_context` through the same
`MPV_RENDER_API_TYPE_OPENGL` path (`mpv/render_gl.h`). Same API, different
target FBO.

## Target shape

```
Ui::RpWidget  (Plazma watch page — built with tdesktop widgets: controls,
   │           seek bar Ui::MediaSlider, hwdec/codec label Ui::FlatLabel)
   └── Plazma::MpvWidget : public QOpenGLWidget
         ├── mpv_handle*          _mpv           (Layer 1 — ported verbatim)
         ├── mpv_render_context*  _renderContext (Layer 2 — rewritten for widgets)
         └── signals: positionChanged/durationChanged/hwdecCurrentChanged/
             videoCodecChanged/… (same names, drive the overlay above)
```

## Layer 2 rewrite — the four QOpenGLWidget hooks

```cpp
void MpvWidget::initializeGL() {
    mpv_opengl_init_params gl{ &MpvWidget::getProcAddress, this };
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    mpv_render_context_create(&_renderContext, _mpv.get(), params);
    // mpv calls this from ITS thread → bounce to the GUI thread via a queued
    // signal, then call QOpenGLWidget::update() there.
    mpv_render_context_set_update_callback(
        _renderContext, &MpvWidget::onUpdate, this);
}

// static — Qt supplies the GL loader; no GLX/EGL/WGL platform code needed.
void *MpvWidget::getProcAddress(void *ctx, const char *name) {
    auto *gl = QOpenGLContext::currentContext();
    return gl ? reinterpret_cast<void*>(gl->getProcAddress(name)) : nullptr;
}

void MpvWidget::paintGL() {
    mpv_opengl_fbo fbo{
        int(defaultFramebufferObject()),
        int(width() * devicePixelRatioF()),
        int(height() * devicePixelRatioF()), 0 };
    int flipY = 1; // QOpenGLWidget's FBO is bottom-left origin like mpv wants
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flipY },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    mpv_render_context_render(_renderContext, params);
}

// static render-callback → queued signal → update() on the GUI thread.
void MpvWidget::onUpdate(void *ctx) {
    QMetaObject::invokeMethod(
        static_cast<MpvWidget*>(ctx), "update", Qt::QueuedConnection);
}
```

That is the *entire* difference from the QQuickFramebufferObject version. No
`MpvRenderer` subclass, no `createRenderer()`, no scene-graph node.

## Threading — it gets SIMPLER, and drop one hazard

`QQuickFramebufferObject::Renderer` runs on the **scene-graph render thread**,
which is why `MpvObject` needs the `MpvCallbackGuard { std::mutex; obj; }` dance
to stop a render-thread callback from racing `~MpvObject` on the GUI thread.

`QOpenGLWidget::paintGL()` runs on the **GUI thread**. So:
- The wakeup callback (`mpv_set_wakeup_callback`) and the render-update callback
  both just post a **queued** signal to the GUI thread — already the pattern the
  event loop uses (`emit onMpvEvents()` → `handleMpvEvents()`).
- The `MpvCallbackGuard` mutex is **no longer needed for the render side** —
  everything touching `_renderContext`/widget state is GUI-thread. Keep a null
  check in the queued slot in case the widget is tearing down.
- **Teardown order matters:** in `~MpvWidget`, call
  `makeCurrent()` → `mpv_render_context_free(_renderContext)` → `doneCurrent()`
  **before** the `mpv_handle` is destroyed, on the GUI thread with the GL
  context current. (The Quick version frees it on the render thread; here it's
  the GUI thread.)

## Load / control — reuse Plazma verbatim

`load(url)`, `play/pause`, `seek`, `setVolume`, `setSpeed`, `setHwdec`,
`takeScreenshot`, the property observers — all `mpv_command`/`mpv_set_property`/
`mpv_observe_property` calls. Copy them straight from `mpv_object.cpp`. Point
`load()` at the `VideoItem::url` returned by `Plazma::Api::fetchVideos`. libmpv
does its own ranged HTTP + hwdec, so **no `Media::Streaming` loader is needed** —
this is why keeping libmpv (rather than porting to tdesktop's FFmpeg pipeline)
is the lower-risk choice, and it preserves the hwdec/codec badge.

## Wiring the "honest decoder" badge

The two observers you pasted are the payload:
```cpp
mpv_observe_property(_mpv.get(), 0, "hwdec-current", MPV_FORMAT_STRING);
mpv_observe_property(_mpv.get(), 0, "video-codec",   MPV_FORMAT_STRING);
```
On `MPV_EVENT_PROPERTY_CHANGE` for those names, emit
`hwdecCurrentChanged`/`videoCodecChanged`; the overlay `Ui::FlatLabel` shows e.g.
`vaapi · h264` or `no · av1` (software). Identical to Plazma's report, now on a
tdesktop label.

## Build wiring

- Link libmpv: add `find_package(PkgConfig)` + `pkg_check_modules(mpv mpv)` (or
  reuse tdesktop's existing FFmpeg link and add `-lmpv`) to the `Telegram`
  target; add `plazma/plazma_mpv_widget.cpp/.h` to its sources.
- Requires an OpenGL-backed `QOpenGLWidget`; tdesktop already ships GL. Confirm
  the widget composits under tdesktop's rounded window (it will — it's a normal
  child widget, unlike the `--wid` embedding path, which we deliberately avoid).

## Decision update vs the report

Report Phase 4 said "drop libmpv, use `Media::Streaming`." Given you want the
decoder readout, prefer: **keep libmpv via `MpvWidget` (QOpenGLWidget)**. It's a
smaller, more faithful port and removes the "arbitrary-URL streaming loader"
unknown. `Media::Streaming` stays a fallback only if libmpv linking is a problem
on a target platform.
