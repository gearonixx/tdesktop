// Plazma port — video feed widget implementation.
#include "plazma/plazma_feed_widget.h"

#include "plazma/plazma_api.h"
#include "plazma/plazma_session.h"

#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "lang/lang_keys.h"
#include "styles/style_widgets.h"
#include "styles/style_dialogs.h"

#include <QtGui/QPainter>

namespace Plazma {
namespace {

constexpr auto kRowHeight = 66;
constexpr auto kPadding = 12;
constexpr auto kThumbW = 88;
constexpr auto kThumbH = 50; // ~16:9
constexpr auto kHeaderHeight = 52;

} // namespace

// ── The scrolling list of video cards ───────────────────────────────────────
class FeedWidget::Inner final : public Ui::RpWidget {
public:
	explicit Inner(QWidget *parent) : Ui::RpWidget(parent) {
	}

	void setVideos(std::vector<VideoItem> videos) {
		_videos = std::move(videos);
		updateHeight();
		update();
	}

	void setStatus(const QString &status) {
		_status = status;
		update();
	}

	void setViewportHeight(int height) {
		_viewportHeight = height;
		updateHeight();
	}

	[[nodiscard]] int count() const {
		return int(_videos.size());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.fillRect(e->rect(), st::dialogsBg);

		if (_videos.empty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				rect().marginsRemoved({ kPadding, kPadding, kPadding, 0 }),
				Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
				_status.isEmpty() ? u"No videos yet."_q : _status);
			return;
		}

		const auto w = width();
		const auto from = std::max(0, e->rect().top() / kRowHeight);
		const auto till = std::min(
			int(_videos.size()),
			(e->rect().bottom() / kRowHeight) + 1);
		for (auto i = from; i != till; ++i) {
			paintRow(p, _videos[i], i * kRowHeight, w);
		}
	}

private:
	void updateHeight() {
		const auto content = int(_videos.size()) * kRowHeight;
		resize(width(), std::max(content, _viewportHeight));
	}

	void paintRow(Painter &p, const VideoItem &v, int top, int w) {
		// Thumbnail placeholder (a rounded purple tile — real thumbnails are a
		// follow-up once an HTTP image loader is wired in).
		const auto thumb = QRect(
			kPadding,
			top + (kRowHeight - kThumbH) / 2,
			kThumbW,
			kThumbH);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgActive);
		p.drawRoundedRect(thumb, 6, 6);

		// A little play glyph so it reads as video.
		const auto cx = thumb.center().x();
		const auto cy = thumb.center().y();
		auto tri = QPainterPath();
		tri.moveTo(cx - 5, cy - 8);
		tri.lineTo(cx - 5, cy + 8);
		tri.lineTo(cx + 9, cy);
		tri.closeSubpath();
		p.fillPath(tri, st::windowFgActive);

		const auto textLeft = thumb.right() + kPadding;
		const auto textWidth = w - textLeft - kPadding;
		if (textWidth <= 0) {
			return;
		}

		p.setPen(st::dialogsNameFg);
		p.setFont(st::semiboldFont);
		const auto title = v.title.isEmpty() ? u"Untitled"_q : v.title;
		p.drawText(
			QRect(textLeft, top + 12, textWidth, st::semiboldFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(title, textWidth));

		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		auto sub = v.author;
		if (!v.createdAt.isEmpty()) {
			sub = sub.isEmpty() ? v.createdAt : (sub + u" · "_q + v.createdAt);
		}
		p.drawText(
			QRect(
				textLeft,
				top + 12 + st::semiboldFont->height + 4,
				textWidth,
				st::normalFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(sub, textWidth));
	}

	std::vector<VideoItem> _videos;
	QString _status;
	int _viewportHeight = 0;

};

// ── FeedWidget ──────────────────────────────────────────────────────────────
FeedWidget::FeedWidget(QWidget *parent, not_null<Main::Session*> session)
: Ui::RpWidget(parent)
, _session(session)
, _scroll(this, st::defaultScrollArea) {
	_inner = _scroll->setOwnedWidget(object_ptr<Inner>(_scroll.data()));

	sizeValue() | rpl::on_next([=](QSize size) {
		const auto viewport = std::max(0, size.height() - kHeaderHeight);
		_scroll->setGeometry(0, kHeaderHeight, size.width(), viewport);
		_inner->resize(size.width(), _inner->height());
		_inner->setViewportHeight(viewport);
	}, lifetime());

	setStatus(u"Connecting to Plazma…"_q);
	setupBackend();
}

FeedWidget::~FeedWidget() = default;

void FeedWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	// Header bar over the feed column.
	const auto header = QRect(0, 0, width(), kHeaderHeight);
	p.fillRect(header, st::dialogsBg);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		header.marginsRemoved({ kPadding + 4, 0, kPadding, 0 }),
		Qt::AlignLeft | Qt::AlignVCenter,
		u"Plazma"_q);
	p.fillRect(0, kHeaderHeight - st::lineWidth, width(), st::lineWidth, st::shadowFg);
}

void FeedWidget::setupBackend() {
	_backend = std::make_unique<Backend>(_session);
	auto *api = &_backend->api();

	QObject::connect(api, &Api::loginSuccess, api, [=](UserLogin) {
		_loggedIn = true;
		loadFeed();
	});
	QObject::connect(api, &Api::loginError, api, [=](int code, QString error) {
		setStatus(u"Plazma login failed (%1). Is the backend running on "
			"localhost:8080?"_q.arg(code));
	});

	_backend->start();
}

void FeedWidget::refresh() {
	if (!_loggedIn) {
		_backend->start();
		return;
	}
	loadFeed();
}

void FeedWidget::loadFeed() {
	if (_loading) {
		return;
	}
	_loading = true;
	setStatus(u"Loading feed…"_q);
	_backend->api().fetchVideos(QString(), [=](QList<VideoItem> videos) {
		_loading = false;
		showVideos({ videos.begin(), videos.end() });
	}, [=](int code, QString error) {
		_loading = false;
		setStatus(u"Could not load feed (HTTP %1)."_q.arg(code));
	});
}

void FeedWidget::showVideos(std::vector<VideoItem> videos) {
	if (videos.empty()) {
		setStatus(u"Your Plazma feed is empty."_q);
	}
	_inner->setVideos(std::move(videos));
}

void FeedWidget::setStatus(const QString &status) {
	_inner->setStatus(status);
	update();
}

} // namespace Plazma
