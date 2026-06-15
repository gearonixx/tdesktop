/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_offline_notes.h"

#include "core/offline_notes.h"
#include "core/mime_type.h"
#include "core/file_location.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/offline/offline_notes_storage.h"
#include "ui/image/image_location_factory.h"

#include "base/unixtime.h"
#include "base/debug_log.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QCryptographicHash>
#include <QtGui/QImage>

namespace Data {

using Storage::Offline::Note;
using Storage::Offline::NotesFolder;

namespace {

// Stable id derived from the media's relative path, so the same note maps to
// the same photo/document object across runs (offline => no server ids exist).
[[nodiscard]] quint64 StableId(const QString &key) {
	const auto digest = QCryptographicHash::hash(
		key.toUtf8(),
		QCryptographicHash::Sha1);
	auto value = quint64(0);
	for (auto i = 0; i != 8; ++i) {
		value = (value << 8) | quint8(digest[i]);
	}
	// Keep it well clear of 0 / small local ids.
	return value | (quint64(1) << 62);
}

// Builds an inline photo whose pixels live entirely in memory, registers it
// with the session (so the message that references it renders without any
// network), and returns the matching message media.
[[nodiscard]] MTPMessageMedia BuildLocalPhoto(
		not_null<Main::Session*> session,
		QImage full,
		PhotoId id) {
	const auto width = full.width();
	const auto height = full.height();
	auto medium = (width > 320 || height > 320)
		? full.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation)
		: full;

	auto thumbs = PreparedPhotoThumbs();
	thumbs.emplace('m', PreparedPhotoThumb{ .image = medium });
	thumbs.emplace('y', PreparedPhotoThumb{ .image = full });

	auto sizes = QVector<MTPPhotoSize>();
	sizes.push_back(MTP_photoSize(
		MTP_string("m"),
		MTP_int(medium.width()),
		MTP_int(medium.height()),
		MTP_int(0)));
	sizes.push_back(MTP_photoSize(
		MTP_string("y"),
		MTP_int(width),
		MTP_int(height),
		MTP_int(0)));

	const auto photo = MTP_photo(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0), // access_hash
		MTP_bytes(), // file_reference
		MTP_int(base::unixtime::now()),
		MTP_vector<MTPPhotoSize>(sizes),
		MTPVector<MTPVideoSize>(),
		MTP_int(0)); // dc_id

	// Seed the in-memory pixels first; the message will re-process the same
	// photo id, but the invalid (dc=0) inline location can't displace a valid
	// in-memory image, so the picture stays displayable.
	session->data().processPhoto(photo, thumbs);

	using Flag = MTPDmessageMediaPhoto::Flag;
	return MTP_messageMediaPhoto(MTP_flags(Flag::f_photo), photo, MTPint());
}

// Builds a document backed by an on-disk file, so non-image attachments open
// straight from notes/media with no network involved.
[[nodiscard]] MTPMessageMedia BuildLocalDocument(
		not_null<Main::Session*> session,
		const QString &absolutePath,
		const QByteArray &bytes,
		const QString &fileName,
		DocumentId id) {
	const auto mime = Core::MimeTypeForFile(
		QFileInfo(absolutePath)).name();
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeFilename(MTP_string(fileName)));

	const auto document = MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0), // access_hash
		MTP_bytes(), // file_reference
		MTP_int(base::unixtime::now()),
		MTP_string(mime),
		MTP_long(bytes.size()),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0), // dc_id
		MTP_vector<MTPDocumentAttribute>(attributes));

	const auto data = session->data().processDocument(document);
	data->setLocation(Core::FileLocation(absolutePath));
	if (!bytes.isEmpty()) {
		data->setDataAndCache(bytes);
	}

	using Flag = MTPDmessageMediaDocument::Flag;
	return MTP_messageMediaDocument(
		MTP_flags(Flag::f_document),
		document,
		MTPVector<MTPDocument>(), // alt_documents
		MTPPhoto(), // video_cover
		MTPint(), // video_timestamp
		MTPint()); // ttl_seconds
}

// Turns a loaded note's first media reference back into displayable message
// media: an inline photo for images, an on-disk document otherwise. Notes
// without media (or with a missing/unreadable file) fall back to empty.
[[nodiscard]] MTPMessageMedia BuildNoteMedia(
		not_null<Main::Session*> session,
		not_null<NotesFolder*> folder,
		const Note &note) {
	if (note.media.empty()) {
		return MTP_messageMediaEmpty();
	}
	const auto relative = note.media.front();
	const auto absolute = folder->mediaAbsolutePath(relative);
	auto file = QFile(absolute);
	if (absolute.isEmpty() || !file.open(QIODevice::ReadOnly)) {
		DEBUG_LOG(("OfflineNotes: media file missing '%1'").arg(absolute));
		return MTP_messageMediaEmpty();
	}
	const auto bytes = file.readAll();
	file.close();

	auto image = QImage();
	if (image.loadFromData(bytes)) {
		return BuildLocalPhoto(
			session,
			std::move(image),
			StableId(relative));
	}
	return BuildLocalDocument(
		session,
		absolute,
		bytes,
		QFileInfo(relative).fileName(),
		StableId(relative));
}

} // namespace

OfflineNotes::OfflineNotes(not_null<Main::Session*> session)
: _session(session) {
	load();

	// NewAdded is only fired for server messages, so new local notes are
	// persisted via noteSent() from the send path. Edited/Destroyed do fire
	// for local messages and are handled here.
	_session->changes().messageUpdates(
		MessageUpdate::Flag::Edited
		| MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const MessageUpdate &update) {
		const auto item = update.item;
		if (!inNotesChat(item)) {
			return;
		}
		if (update.flags & MessageUpdate::Flag::Destroyed) {
			persistRemove(item);
		} else if (update.flags & MessageUpdate::Flag::Edited) {
			persistEdit(item);
		}
	}, _lifetime);
}

std::vector<FullMsgId> OfflineNotes::search(const QString &query) const {
	auto result = std::vector<FullMsgId>();
	const auto trimmed = query.trimmed();
	if (trimmed.isEmpty()) {
		return result;
	}
	for (const auto &[item, id] : _noteIds) {
		if (item->originalText().text.contains(
				trimmed,
				Qt::CaseInsensitive)) {
			result.push_back(item->fullId());
		}
	}
	// Newest first, matching the server search ordering.
	std::sort(result.begin(), result.end(), [](FullMsgId a, FullMsgId b) {
		return a.msg > b.msg;
	});
	return result;
}

void OfflineNotes::noteSentWithMedia(
		not_null<HistoryItem*> item,
		const QByteArray &content,
		const QString &filepath,
		const QString &filename) {
	if (!inNotesChat(item) || _noteIds.contains(item)) {
		return;
	}
	auto rel = filepath.isEmpty()
		? _folder->saveMediaBytes(content, filename)
		: _folder->importMedia(filepath);
	auto note = noteFrom(item);
	if (!rel.isEmpty()) {
		note.media.push_back(rel);
	}
	DEBUG_LOG(("OfflineNotes: noteSentWithMedia rel='%1' caption='%2'"
		).arg(rel).arg(note.text.left(20)));
	const auto id = _folder->append(std::move(note));
	if (!id.isEmpty()) {
		_noteIds.emplace(item, id);
	}
}

void OfflineNotes::noteSent(not_null<HistoryItem*> item) {
	DEBUG_LOG(("OfflineNotes: noteSent inChat=%1 mapped=%2"
		).arg(inNotesChat(item) ? 1 : 0
		).arg(_noteIds.contains(item) ? 1 : 0));
	if (inNotesChat(item)) {
		persistNew(item);
	}
}

OfflineNotes::~OfflineNotes() = default;

void OfflineNotes::load() {
	_folder = std::make_unique<NotesFolder>(Core::OfflineNotes::FolderPath());
	_folder->ensureReady();

	_history = _session->data().history(_session->user());

	// Give the single chat its fixed "Locker" star avatar. Offline => set it
	// straight from the embedded PNG via an in-memory image location, so it
	// renders everywhere the peer userpic is drawn without any up/download.
	if (const auto self = _session->user()) {
		const auto bytes = Core::OfflineNotes::AvatarData();
		auto image = QImage();
		if (image.loadFromData(bytes)) {
			constexpr auto kAvatarPhotoId = PhotoId(0x10C5E70000000001ULL);
			self->setUserpic(
				kAvatarPhotoId,
				Images::FromImageInMemory(image, "PNG", bytes).location,
				false);
		}
	}

	const auto notes = _folder->load();
	_loading = true;
	auto withMedia = 0;
	for (const auto &note : notes) {
		auto flags = MessageFlags(MessageFlag::HasFromId)
			| (note.pinned ? MessageFlag::Pinned : MessageFlag());
		auto media = BuildNoteMedia(_session, _folder.get(), note);
		if (media.type() != mtpc_messageMediaEmpty) {
			++withMedia;
		}
		const auto item = _history->addNewLocalMessage({
			.id = _session->data().nextLocalMessageId(),
			.flags = flags,
			.from = _session->userPeerId(),
			.date = TimeId(note.date),
		}, TextWithEntities{ note.text }, media);
		_noteIds.emplace(item, note.id);
	}
	_loading = false;
	DEBUG_LOG(("OfflineNotes: reattached media on %1 of %2 notes"
		).arg(withMedia).arg(notes.size()));
	DEBUG_LOG(("OfflineNotes: loaded %1 notes into history=%2 from %3"
		).arg(notes.size()
		).arg(_history ? 1 : 0
		).arg(_folder->folderPath()));
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
	DEBUG_LOG(("OfflineNotes: persistNew text='%1' -> id='%2'"
		).arg(item->originalText().text.left(20)).arg(id));
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
	// noteFrom() only carries text/date; keep the media and reactions that
	// were stored when the note was first created, so editing a caption does
	// not drop the attached image.
	for (const auto &existing : _folder->load()) {
		if (existing.id == note.id) {
			note.media = existing.media;
			note.reactions = existing.reactions;
			break;
		}
	}
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
