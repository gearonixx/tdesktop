// Plazma port — playlists ("video folders") controller implementation.
//
// Optimistic-then-reconcile, ported from Plazma's playlists_model.cpp, but
// lifetime-safe (crl::guard on every async reply), reactive (rpl outputs), and
// with a membership index so "is this video saved" is correct independent of
// which folders have had their items lazily loaded.
#include "plazma/plazma_playlists.h"

#include "plazma/plazma_api.h"

#include <QtCore/QUuid>
#include <QtCore/QDateTime>

#include <algorithm>

namespace Plazma {
namespace {

constexpr auto kPreviewThumbs = 4;
constexpr auto kMaxNameLength = 100;

} // namespace

Playlists::Playlists(not_null<Api*> api)
: _api(api) {
}

// ── Lookup helpers ──────────────────────────────────────────────────────────

int Playlists::indexOf(const QString &id) const {
	for (auto i = 0; i != int(_playlists.size()); ++i) {
		if (_playlists[i].id == id) {
			return i;
		}
	}
	return -1;
}

const Playlist *Playlists::find(const QString &id) const {
	const auto i = indexOf(id);
	return (i >= 0) ? &_playlists[i] : nullptr;
}

Playlist *Playlists::findMutable(const QString &id) {
	const auto i = indexOf(id);
	return (i >= 0) ? &_playlists[i] : nullptr;
}

QString Playlists::name(const QString &id) const {
	const auto p = find(id);
	return p ? p->name : QString();
}

void Playlists::sortByName() {
	std::sort(
		_playlists.begin(),
		_playlists.end(),
		[](const Playlist &a, const Playlist &b) {
			const auto c = a.name.localeAwareCompare(b.name);
			return (c != 0) ? (c < 0) : (a.id < b.id);
		});
}

void Playlists::touch(Playlist &p) {
	p.updatedAt = isoNowUtc();
}

void Playlists::notifyChanged() {
	_changes.fire({});
}

void Playlists::notifyCurrentChanged(const QString &playlistId) {
	if (playlistId == _currentId) {
		_currentChanges.fire({});
	}
}

// ── Membership index (videoId → playlist ids) ───────────────────────────────

void Playlists::indexMembership(
		const QString &playlistId,
		const QString &videoId) {
	if (!videoId.isEmpty()) {
		_membership[videoId].insert(playlistId);
	}
}

void Playlists::deindexMembership(
		const QString &playlistId,
		const QString &videoId) {
	const auto i = _membership.find(videoId);
	if (i != _membership.end()) {
		i->second.erase(playlistId);
		if (i->second.empty()) {
			_membership.erase(i);
		}
	}
}

void Playlists::reindexFromItems(const Playlist &p) {
	for (const auto &item : p.items) {
		indexMembership(p.id, item.videoId);
	}
}

void Playlists::dropFromMembership(const QString &playlistId) {
	for (auto i = _membership.begin(); i != _membership.end();) {
		i->second.erase(playlistId);
		i = i->second.empty() ? _membership.erase(i) : std::next(i);
	}
}

// ── Static converters ───────────────────────────────────────────────────────

PlaylistItem Playlists::itemFromVideo(const VideoItem &video) {
	auto item = PlaylistItem();
	item.videoId = video.id;
	item.title = video.title;
	item.url = video.url;
	item.thumbnail = video.thumbnail;
	item.storyboard = video.storyboard;
	item.author = video.author;
	item.mime = video.mime;
	item.description = video.description;
	item.size = video.size;
	item.durationMs = -1;
	item.addedAt = isoNowUtc();
	return item;
}

QString Playlists::makeId() {
	// Plazma uses UUID v7 for time-ordering; a v4 is fine for the client id
	// (the server orders by created_at) and stays dependency-free.
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Playlists::isoNowUtc() {
	return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// ── Validation & queries ────────────────────────────────────────────────────

bool Playlists::isValidName(const QString &name) const {
	const auto trimmed = name.trimmed();
	return !trimmed.isEmpty() && (trimmed.length() <= kMaxNameLength);
}

bool Playlists::isNameTaken(const QString &name, const QString &exceptId) const {
	const auto trimmed = name.trimmed();
	for (const auto &p : _playlists) {
		if (p.id != exceptId
			&& p.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

bool Playlists::contains(
		const QString &playlistId,
		const QString &videoId) const {
	const auto i = _membership.find(videoId);
	return (i != _membership.end()) && i->second.contains(playlistId);
}

QStringList Playlists::playlistsContaining(const QString &videoId) const {
	auto result = QStringList();
	const auto i = _membership.find(videoId);
	if (i != _membership.end()) {
		for (const auto &id : i->second) {
			result.push_back(id);
		}
	}
	return result;
}

QList<PlaylistSummary> Playlists::summariesForVideo(
		const QString &videoId) const {
	const auto i = _membership.find(videoId);
	const auto *ids = (i != _membership.end()) ? &i->second : nullptr;
	auto out = QList<PlaylistSummary>();
	out.reserve(int(_playlists.size()));
	for (const auto &p : _playlists) {
		auto summary = PlaylistSummary();
		summary.id = p.id;
		summary.name = p.name;
		summary.itemCount = p.itemCount;
		summary.contains = ids && ids->contains(p.id);
		out.push_back(summary);
	}
	return out;
}

void Playlists::ensureMembership(const QString &videoId) {
	if (videoId.isEmpty()) {
		return;
	}
	_api->playlistsByVideo(videoId, crl::guard(this, [=](QStringList ids) {
		// Server is authoritative for this video's membership — replace.
		auto &set = _membership[videoId];
		set.clear();
		for (const auto &id : ids) {
			set.insert(id);
		}
		if (set.empty()) {
			_membership.erase(videoId);
		}
		notifyChanged();
	}));
}

QStringList Playlists::coverThumbnails(const QString &id) const {
	const auto p = find(id);
	if (!p) {
		return {};
	}
	if (!p->coverThumbnails.isEmpty()) {
		return p->coverThumbnails;
	}
	auto out = QStringList();
	for (const auto &item : p->items) {
		if (!item.thumbnail.isEmpty()) {
			out.push_back(item.thumbnail);
			if (out.size() >= kPreviewThumbs) {
				break;
			}
		}
	}
	return out;
}

// ── Mutations ───────────────────────────────────────────────────────────────

QString Playlists::create(const QString &name) {
	const auto trimmed = name.trimmed();
	if (!isValidName(trimmed) || isNameTaken(trimmed)) {
		_errors.fire(u"Choose a different name"_q);
		return QString();
	}

	const auto id = makeId();
	auto playlist = Playlist();
	playlist.id = id;
	playlist.name = trimmed;
	playlist.createdAt = isoNowUtc();
	playlist.updatedAt = playlist.createdAt;
	playlist.itemCount = 0;
	_playlists.push_back(std::move(playlist));
	sortByName();
	notifyChanged();

	_lastCreatedId = id;

	_api->createPlaylist(id, trimmed, crl::guard(this, [=](Playlist server) {
		reconcileSingle(std::move(server));
	}), crl::guard(this, [=](int code, QString error) {
		if (const auto row = indexOf(id); row >= 0) {
			_playlists.erase(_playlists.begin() + row);
			notifyChanged();
		}
		_errors.fire(error.isEmpty() ? u"Could not create playlist"_q : error);
	}));
	return id;
}

bool Playlists::rename(const QString &id, const QString &newName) {
	const auto p = findMutable(id);
	const auto trimmed = newName.trimmed();
	if (!p || !isValidName(trimmed) || isNameTaken(trimmed, id)) {
		return false;
	}
	if (p->name == trimmed) {
		return true;
	}

	const auto previousName = p->name;
	const auto previousUpdatedAt = p->updatedAt;
	p->name = trimmed;
	touch(*p);
	sortByName();
	notifyChanged();
	notifyCurrentChanged(id);

	_api->renamePlaylist(id, trimmed, crl::guard(this, [=](Playlist server) {
		reconcileSingle(std::move(server));
	}), crl::guard(this, [=](int code, QString error) {
		if (auto p = findMutable(id)) {
			p->name = previousName;
			p->updatedAt = previousUpdatedAt;
			sortByName();
			notifyChanged();
			notifyCurrentChanged(id);
		}
		_errors.fire(error.isEmpty() ? u"Could not rename playlist"_q : error);
	}));
	return true;
}

bool Playlists::remove(const QString &id) {
	const auto row = indexOf(id);
	if (row < 0) {
		return false;
	}

	const auto snapshot = _playlists[row];
	dropFromMembership(id);
	_playlists.erase(_playlists.begin() + row);
	notifyChanged();
	if (_currentId == id) {
		_currentId.clear();
		_currentChanges.fire({});
	}

	_api->deletePlaylist(id, crl::guard(this, [] {
		// Deleted; nothing to reconcile.
	}), crl::guard(this, [=](int code, QString error) {
		_playlists.push_back(snapshot);
		reindexFromItems(snapshot);
		sortByName();
		notifyChanged();
		_errors.fire(error.isEmpty() ? u"Could not delete playlist"_q : error);
	}));
	return true;
}

bool Playlists::addVideo(const QString &playlistId, const VideoItem &video) {
	const auto p = findMutable(playlistId);
	if (!p) {
		return false;
	}
	if (contains(playlistId, video.id)
		|| (video.id.isEmpty() && !video.url.isEmpty() && std::any_of(
			p->items.begin(),
			p->items.end(),
			[&](const PlaylistItem &i) { return i.url == video.url; }))) {
		_errors.fire(u"Already in “%1”"_q.arg(p->name));
		return false;
	}

	const auto item = itemFromVideo(video);
	p->items.push_back(item);
	p->itemCount = std::max(p->itemCount + 1, int(p->items.size()));
	indexMembership(playlistId, video.id);
	touch(*p);
	notifyChanged();
	notifyCurrentChanged(playlistId);

	_api->addPlaylistItem(playlistId, item, crl::guard(this, [=](
			PlaylistItem canonical,
			Playlist summary) {
		if (auto p = findMutable(playlistId)) {
			for (auto &existing : p->items) {
				if (existing.videoId == canonical.videoId) {
					existing = canonical;
					break;
				}
			}
		}
		if (!summary.id.isEmpty()) {
			reconcileSingle(std::move(summary));
		}
		notifyCurrentChanged(playlistId);
	}), crl::guard(this, [=, videoId = video.id](int code, QString error) {
		if (auto p = findMutable(playlistId); p && !videoId.isEmpty()) {
			const auto last = std::remove_if(
				p->items.begin(),
				p->items.end(),
				[&](const PlaylistItem &i) { return i.videoId == videoId; });
			p->items.erase(last, p->items.end());
			p->itemCount = std::max(0, p->itemCount - 1);
			deindexMembership(playlistId, videoId);
			notifyChanged();
			notifyCurrentChanged(playlistId);
		}
		_errors.fire(error.isEmpty() ? u"Could not save video"_q : error);
	}));
	return true;
}

bool Playlists::removeVideo(
		const QString &playlistId,
		const QString &videoId) {
	const auto p = findMutable(playlistId);
	if (!p) {
		return false;
	}
	const auto i = std::find_if(
		p->items.begin(),
		p->items.end(),
		[&](const PlaylistItem &it) { return it.videoId == videoId; });
	if (i == p->items.end()) {
		return false;
	}

	const auto snapshot = *i;
	p->items.erase(i);
	p->itemCount = std::max(0, p->itemCount - 1);
	deindexMembership(playlistId, videoId);
	touch(*p);
	notifyChanged();
	notifyCurrentChanged(playlistId);

	_api->removePlaylistItem(playlistId, videoId, crl::guard(this, [] {
		// Removed; nothing to reconcile.
	}), crl::guard(this, [=](int code, QString error) {
		if (auto p = findMutable(playlistId)) {
			p->items.push_back(snapshot);
			p->itemCount = std::max(p->itemCount + 1, int(p->items.size()));
			indexMembership(playlistId, snapshot.videoId);
			notifyChanged();
			notifyCurrentChanged(playlistId);
		}
		_errors.fire(error.isEmpty() ? u"Could not remove video"_q : error);
	}));
	return true;
}

// ── The open playlist (detail view) ─────────────────────────────────────────

void Playlists::openPlaylist(const QString &id) {
	if (_currentId == id) {
		return;
	}
	const auto p = find(id);
	if (!p) {
		return;
	}
	_currentId = id;
	_currentChanges.fire({});

	// Lazily hydrate items — the list endpoint returns only summaries.
	if (int(p->items.size()) < p->itemCount) {
		_api->getPlaylist(id, crl::guard(this, [=](Playlist detail) {
			reconcileItems(id, detail.items);
			if (auto p = findMutable(id)) {
				if (!detail.coverThumbnails.isEmpty()) {
					p->coverThumbnails = detail.coverThumbnails;
				}
				if (detail.itemCount > 0) {
					p->itemCount = detail.itemCount;
				}
			}
			notifyCurrentChanged(id);
		}), crl::guard(this, [=](int code, QString error) {
			_errors.fire(std::move(error));
		}));
	}
}

void Playlists::closeCurrent() {
	if (_currentId.isEmpty()) {
		return;
	}
	_currentId.clear();
	_currentChanges.fire({});
}

// ── Refresh & reconcile ─────────────────────────────────────────────────────

void Playlists::refresh() {
	if (_refreshInFlight) {
		_refreshQueued = true;
		return;
	}
	_refreshInFlight = true;
	_loading = true;

	_api->listPlaylists(crl::guard(this, [=](QList<Playlist> list) {
		reconcileSummaries(list);
		_refreshInFlight = false;
		_loading = false;
		if (_refreshQueued) {
			_refreshQueued = false;
			refresh();
		}
	}), crl::guard(this, [=](int code, QString error) {
		_refreshInFlight = false;
		_loading = false;
		if (_refreshQueued) {
			_refreshQueued = false;
			refresh();
		}
		// code==0 (offline) during boot is expected; don't spam a banner.
		if (code != 0) {
			_errors.fire(
				error.isEmpty() ? u"Could not refresh playlists"_q : error);
		}
	}));
}

void Playlists::reconcileSummaries(const QList<Playlist> &summaries) {
	// The list endpoint has no items; carry over any we already loaded so the
	// detail view / cover mosaics don't flicker empty. Anything the server
	// didn't return has been deleted elsewhere — drop it.
	auto next = std::vector<Playlist>();
	next.reserve(summaries.size());
	for (auto summary : summaries) {
		if (summary.id.isEmpty()) {
			continue;
		}
		if (const auto prev = find(summary.id)) {
			summary.items = prev->items;
		}
		next.push_back(std::move(summary));
	}
	_playlists = std::move(next);
	sortByName();

	// Rebuild membership from loaded items; per-video server truth is
	// re-hydrated lazily by ensureMembership() when a dialog needs it.
	_membership.clear();
	for (const auto &p : _playlists) {
		reindexFromItems(p);
	}
	notifyChanged();

	if (!_currentId.isEmpty() && !find(_currentId)) {
		_currentId.clear();
		_currentChanges.fire({});
	}
}

void Playlists::reconcileSingle(Playlist summary) {
	if (summary.id.isEmpty()) {
		return;
	}
	const auto id = summary.id;
	if (auto p = findMutable(id)) {
		if (summary.items.isEmpty()) {
			summary.items = p->items; // summary carries no items; preserve.
		}
		*p = std::move(summary);
		reindexFromItems(*p);
		sortByName(); // invalidates which playlist `p` points at.
		notifyChanged();
		notifyCurrentChanged(id);
	} else {
		_playlists.push_back(std::move(summary));
		reindexFromItems(_playlists.back());
		sortByName();
		notifyChanged();
	}
}

void Playlists::reconcileItems(
		const QString &playlistId,
		QList<PlaylistItem> items) {
	auto p = findMutable(playlistId);
	if (!p) {
		return;
	}
	p->items = std::move(items);
	p->itemCount = std::max(p->itemCount, int(p->items.size()));
	reindexFromItems(*p);
	notifyChanged();
	notifyCurrentChanged(playlistId);
}

} // namespace Plazma
