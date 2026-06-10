/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_offline_notes.h"

#include "core/offline_notes.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/offline/offline_notes_storage.h"

#include "base/unixtime.h"

namespace Data {

using Storage::Offline::Note;
using Storage::Offline::NotesFolder;

OfflineNotes::OfflineNotes(not_null<Main::Session*> session)
: _session(session) {
	load();

	_session->changes().messageUpdates(
		MessageUpdate::Flag::NewAdded
		| MessageUpdate::Flag::Edited
		| MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const MessageUpdate &update) {
		const auto item = update.item;
		if (!inNotesChat(item)) {
			return;
		}
		if (update.flags & MessageUpdate::Flag::Destroyed) {
			persistRemove(item);
		} else if (update.flags & MessageUpdate::Flag::NewAdded) {
			if (!_loading) {
				persistNew(item);
			}
		} else if (update.flags & MessageUpdate::Flag::Edited) {
			persistEdit(item);
		}
	}, _lifetime);
}

OfflineNotes::~OfflineNotes() = default;

void OfflineNotes::load() {
	_folder = std::make_unique<NotesFolder>(Core::OfflineNotes::FolderPath());
	_folder->ensureReady();

	_history = _session->data().history(_session->user());

	const auto notes = _folder->load();
	_loading = true;
	for (const auto &note : notes) {
		auto flags = MessageFlags(MessageFlag::HasFromId)
			| (note.pinned ? MessageFlag::Pinned : MessageFlag());
		const auto item = _history->addNewLocalMessage({
			.id = _session->data().nextLocalMessageId(),
			.flags = flags,
			.from = _session->userPeerId(),
			.date = TimeId(note.date),
		}, TextWithEntities{ note.text }, MTP_messageMediaEmpty());
		_noteIds.emplace(item, note.id);
	}
	_loading = false;
}

bool OfflineNotes::inNotesChat(not_null<HistoryItem*> item) const {
	return _history && (item->history() == _history);
}

Note OfflineNotes::noteFrom(not_null<HistoryItem*> item) const {
	auto note = Note();
	note.date = item->date();
	note.text = item->originalText().text;
	note.pinned = item->isPinned();
	return note;
}

void OfflineNotes::persistNew(not_null<HistoryItem*> item) {
	if (_noteIds.contains(item)) {
		return;
	}
	auto note = noteFrom(item);
	const auto id = _folder->append(std::move(note));
	if (!id.isEmpty()) {
		_noteIds.emplace(item, id);
	}
}

void OfflineNotes::persistEdit(not_null<HistoryItem*> item) {
	const auto i = _noteIds.find(item);
	if (i == end(_noteIds)) {
		return;
	}
	auto note = noteFrom(item);
	note.id = i->second;
	note.edited = base::unixtime::now();
	_folder->update(note);
}

void OfflineNotes::persistRemove(not_null<HistoryItem*> item) {
	const auto i = _noteIds.find(item);
	if (i == end(_noteIds)) {
		return;
	}
	_folder->remove(i->second);
	_noteIds.erase(i);
}

} // namespace Data
