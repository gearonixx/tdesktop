// Plazma port — libmpv playback surface for Telegram Desktop.
//
// Layer 1 (mpv_handle + event/property loop) is ported 1:1 from Plazma's
// src/ui/mpv_object.{h,cpp}. Layer 2 (the render surface) is rewritten from a
// QQuickFramebufferObject to a QOpenGLWidget so it composes inside tdesktop's
// Qt Widgets scene. See dev/plazma/mpv_integration.md for the rationale.
#pragma once

#include <QtOpenGLWidgets/QOpenGLWidget>
#include <memory>

struct mpv_handle;
struct mpv_render_context;

namespace Plazma {

class MpvWidget final : public QOpenGLWidget {
	Q_OBJECT

public:
	explicit MpvWidget(QWidget *parent = nullptr);
	~MpvWidget() override;

	// Control surface — thin wrappers over mpv_command / mpv_set_property,
	// ported verbatim from MpvObject.
	void load(const QString &url);
	void stop();
	void playPause();
	void seek(double seconds);
	void seekRelative(double deltaSeconds);
	void setVolume(double v);
	void setMuted(bool muted);
	void setSpeed(double s);
	void setHwdec(const QString &mode); // "auto-safe" default, like Plazma

	[[nodiscard]] bool paused() const { return _paused; }
	[[nodiscard]] double duration() const { return _duration; }
	[[nodiscard]] double position() const { return _position; }
	[[nodiscard]] double bufferedPosition() const { return _bufferedPosition; }
	// The "honest decoder" readout Plazma advertises.
	[[nodiscard]] QString hwdecCurrent() const { return _hwdecCurrent; }
	[[nodiscard]] QString videoCodec() const { return _videoCodec; }

Q_SIGNALS:
	void pausedChanged();
	void durationChanged();
	void positionChanged();
	void bufferedPositionChanged();
	void hwdecCurrentChanged();
	void videoCodecChanged();
	void fileLoaded();
	void endReached();
	void playbackError(QString message);

protected:
	// Layer 2 — the only code that differs from the Qt Quick version.
	void initializeGL() override;
	void paintGL() override;

private:
	static void *getProcAddress(void *ctx, const char *name);
	static void onMpvWakeup(void *ctx);   // → queued signal → handleMpvEvents()
	static void onMpvRenderUpdate(void *ctx); // → queued invoke of update()
	void handleMpvEvents();               // the switch you pasted, verbatim

	std::shared_ptr<mpv_handle> _mpv;
	mpv_render_context *_renderContext = nullptr;

	bool _paused = true;
	double _duration = 0.;
	double _position = 0.;
	double _bufferedPosition = 0.;
	QString _hwdec = QStringLiteral("auto-safe");
	QString _hwdecCurrent;
	QString _videoCodec;
};

} // namespace Plazma
