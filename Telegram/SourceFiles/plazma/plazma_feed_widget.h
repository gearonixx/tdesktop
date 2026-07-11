// Plazma port — the video feed that replaces the chat list.
//
// A self-contained left-column surface: it logs into PlazmaServer using the
// current Telegram session (via Plazma::Backend), fetches GET /v1/videos and
// paints the videos as a scrollable list of cards. Dropped over MainWidget's
// `_dialogs` column so opening Telegram shows the feed instead of chats.
//
// Deliberately standalone-ish: it owns its own Api/Backend so the only edit to
// core tdesktop is constructing + positioning this widget in MainWidget.
#pragma once

#include "ui/rp_widget.h"

#include "base/object_ptr.h"

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class ScrollArea;
} // namespace Ui

namespace Plazma {

class Backend;
struct VideoItem;

class FeedWidget final : public Ui::RpWidget {
public:
	FeedWidget(QWidget *parent, not_null<Main::Session*> session);
	~FeedWidget();

	// Re-fetch the feed from the backend (also re-runs login if needed).
	void refresh();

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	class Inner;

	void setupBackend();
	void loadFeed();
	void showVideos(std::vector<VideoItem> videos);
	void setStatus(const QString &status);

	const not_null<Main::Session*> _session;
	std::unique_ptr<Backend> _backend;

	object_ptr<Ui::ScrollArea> _scroll;
	Inner *_inner = nullptr;

	bool _loggedIn = false;
	bool _loading = false;

};

} // namespace Plazma
