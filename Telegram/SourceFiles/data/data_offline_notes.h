/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "data/data_msg_id.h"

#include <rpl/lifetime.h>
#include <memory>

class History;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Storage::Offline {
class NotesFolder;
struct Note;
} // namespace Storage::Offline

namespace Data {

// Owns the bridge between the in-memory self chat and the on-disk Markdown
// note store (storage/offline/). On construction it loads the folder's notes
// into the single chat; thereafter it mirrors every message add / edit /
// delete back to disk. There is no network involved.
class OfflineNotes final {
public:
	explicit OfflineNotes(not_null<Main::Session*> session);
	~OfflineNotes();

	// Called from the offline send path for a just-created local message:
	// persists it as a new note. addNewLocalMessage does not fire a NewAdded
	// update, so this is driven directly rather than via an observer.
	void noteSent(not_null<HistoryItem*> item);

	// Like noteSent, but also saves the media file into media/ and records its
	// path in the note. Pass either a filepath or raw content (e.g. a paste).
	void noteSentWithMedia(
		not_null<HistoryItem*> item,
		const QByteArray &content,
		const QString &filepath,
		const QString &filename);

	// Local full-text search over the notes (offline replacement for the
	// server messages.search). Returns matching message ids, newest first.
	[[nodiscard]] std::vector<FullMsgId> search(const QString &query) const;

private:
	void load();
	void persistNew(not_null<HistoryItem*> item);
	void persistEdit(not_null<HistoryItem*> item);
	void persistRemove(not_null<HistoryItem*> item);

	[[nodiscard]] bool inNotesChat(not_null<HistoryItem*> item) const;
	[[nodiscard]] Storage::Offline::Note noteFrom(
		not_null<HistoryItem*> item) const;

	const not_null<Main::Session*> _session;
	std::unique_ptr<Storage::Offline::NotesFolder> _folder;
	History *_history = nullptr;

	// Keyed by item pointer (stable across local->server id promotion), so
	// edits and deletes find the note they came from.
	base::flat_map<HistoryItem*, QString> _noteIds;

	// True only while seeding the chat from disk, so loaded messages aren't
	// re-persisted as brand new notes.
	bool _loading = false;

	rpl::lifetime _lifetime;

};

} // namespace Data
