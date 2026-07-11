// Plazma port — playlists ("video folders") controller.
//
// Faithful port of Plazma's src/models/playlists_model.{h,cpp}, rebuilt around
// tdesktop idioms rather than QAbstractListModel/QML:
//
//   * lifetime-safe: derives base::has_weak_ptr and wraps every async Api
//     callback in crl::guard(this, …), so a destroyed controller simply drops
//     an in-flight reply instead of dereferencing freed memory;
//   * reactive: change notification is via rpl producers (changes(),
//     currentChanges(), loadingValue(), errors()), the way tdesktop widgets
//     bind — no Qt signals;
//   * correct membership: "is this video saved / in which folders" is answered
//     from a dedicated index (videoId → playlistIds) fed by mutations AND the
//     GET .../by_video endpoint, so it does NOT depend on a folder's items
//     being lazily loaded (the latent bug in a naive items-scan).
//
// Optimistic-then-reconcile semantics and the wire contract are unchanged:
// PlazmaServer/docs/playlists.md. UI mapping (folder tabs, NOT Data::ChatFilters
// which is peer-centric): dev/plazma/DECISION_playlists_folders.md.
#pragma once

#include "plazma/plazma_api.h"

#include "base/weak_ptr.h"

#include "rpl/event_stream.h"
#include "rpl/producer.h"
#include "rpl/variable.h"

#include <map>
#include <set>
#include <vector>

namespace Plazma {

class Api;

// One row for the "Save to playlist" submenu: every folder + whether it already
// holds the video (mirrors Plazma's summariesForVideo).
struct PlaylistSummary final {
	QString id;
	QString name;
	int itemCount = 0;
	bool contains = false;
};

class Playlists final : public base::has_weak_ptr {
public:
	explicit Playlists(not_null<Api*> api);

	// ── The list (synchronous reads from the local mirror) ────────────────
	[[nodiscard]] const std::vector<Playlist> &all() const {
		return _playlists;
	}
	[[nodiscard]] int count() const {
		return int(_playlists.size());
	}
	[[nodiscard]] bool loading() const {
		return _loading.current();
	}
	[[nodiscard]] const Playlist *find(const QString &id) const;
	[[nodiscard]] QString name(const QString &id) const;
	[[nodiscard]] QString lastCreatedId() const {
		return _lastCreatedId;
	}

	// ── Mutations (optimistic + reconcile, rollback on failure) ───────────
	// create() is synchronous: validates, generates a client id, inserts the
	// folder locally, registers it server-side in the background. Returns the
	// new id, or an empty string if the name is invalid/taken.
	QString create(const QString &name);
	bool rename(const QString &id, const QString &newName);
	bool remove(const QString &id);
	// addVideo is idempotent: false if already present (checked via membership).
	bool addVideo(const QString &playlistId, const VideoItem &video);
	bool removeVideo(const QString &playlistId, const QString &videoId);

	// ── Membership queries (correct even when items aren't hydrated) ──────
	[[nodiscard]] bool contains(
		const QString &playlistId,
		const QString &videoId) const;
	[[nodiscard]] QStringList playlistsContaining(
		const QString &videoId) const;
	[[nodiscard]] QList<PlaylistSummary> summariesForVideo(
		const QString &videoId) const;
	// Hydrate membership for one video from the server (GET .../by_video). The
	// "Save to playlist" dialog calls this on open so checkmarks are correct
	// without every folder's items being loaded. Fires changes() when it lands.
	void ensureMembership(const QString &videoId);

	// ── Rendering helpers ─────────────────────────────────────────────────
	[[nodiscard]] QStringList coverThumbnails(const QString &id) const;
	[[nodiscard]] bool isValidName(const QString &name) const;
	[[nodiscard]] bool isNameTaken(
		const QString &name,
		const QString &exceptId = {}) const;

	// ── The open playlist (detail view); lazily fetches items ─────────────
	void openPlaylist(const QString &id);
	void closeCurrent();
	[[nodiscard]] QString currentId() const {
		return _currentId;
	}
	[[nodiscard]] const Playlist *currentPlaylist() const {
		return find(_currentId);
	}

	// Pulls the server snapshot and reconciles the mirror (coalesced).
	void refresh();

	// ── Reactive outputs (bind widgets to these) ──────────────────────────
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}
	[[nodiscard]] rpl::producer<> currentChanges() const {
		return _currentChanges.events();
	}
	[[nodiscard]] rpl::producer<bool> loadingValue() const {
		return _loading.value();
	}
	[[nodiscard]] rpl::producer<QString> errors() const {
		return _errors.events();
	}

private:
	[[nodiscard]] Playlist *findMutable(const QString &id);
	[[nodiscard]] int indexOf(const QString &id) const;
	void sortByName();
	void touch(Playlist &p);
	void notifyChanged();
	void notifyCurrentChanged(const QString &playlistId);

	// Membership index maintenance.
	void indexMembership(const QString &playlistId, const QString &videoId);
	void deindexMembership(const QString &playlistId, const QString &videoId);
	void reindexFromItems(const Playlist &p);
	void dropFromMembership(const QString &playlistId);

	// Reconcile: the list endpoint returns summaries WITHOUT items, so we carry
	// over already-loaded items for matching ids.
	void reconcileSummaries(const QList<Playlist> &summaries);
	void reconcileSingle(Playlist summary);
	void reconcileItems(const QString &playlistId, QList<PlaylistItem> items);

	[[nodiscard]] static PlaylistItem itemFromVideo(const VideoItem &video);
	[[nodiscard]] static QString makeId();
	[[nodiscard]] static QString isoNowUtc();

	const not_null<Api*> _api;
	std::vector<Playlist> _playlists;

	// videoId → the set of playlist ids known to contain it. Authoritative for
	// membership queries; fed by mutations, item loads, and .../by_video.
	base::flat_map<QString, base::flat_set<QString>> _membership;

	QString _currentId;
	QString _lastCreatedId;
	bool _refreshInFlight = false;
	bool _refreshQueued = false;

	rpl::variable<bool> _loading = false;
	rpl::event_stream<> _changes;
	rpl::event_stream<> _currentChanges;
	rpl::event_stream<QString> _errors;
};

} // namespace Plazma
