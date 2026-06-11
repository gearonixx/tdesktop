/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace Storage::Offline {

// One reaction on a note: an emoji and how many times it was applied.
struct NoteReaction {
	QString emoji;
	int count = 1;
};

// A single note. Maps onto a Telegram message: a chunk of CommonMark text,
// optional media attachments (paths relative to the notes folder), pin flag,
// reactions, and creation / last-edit timestamps (Unix seconds, UTC).
struct Note {
	QString id;
	qint64 date = 0;
	qint64 edited = 0;
	bool pinned = false;
	QString text;
	std::vector<QString> media;
	std::vector<NoteReaction> reactions;

	[[nodiscard]] bool isEdited() const {
		return edited > date;
	}
};

// File-backed store for the single, nameless offline chat.
//
// On disk a folder holds:
//   messages.md   - all notes, newest appended last, fully round-trippable
//   media/        - imported photos / videos / files referenced by notes
//
// The .md format keeps each note's metadata in an HTML comment (invisible in
// Obsidian / any Markdown renderer) wrapping the CommonMark body:
//
//   <!--note id="..." date="..." edited="..." pinned="1"
//            reactions="emoji:count,..." media="rel/path|rel/path"-->
//   Body text in CommonMark.
//   <!--/note-->
//
// All operations rewrite messages.md atomically; the store is small (a personal
// note log), so simplicity and durability win over incremental writes.
class NotesFolder final {
public:
	explicit NotesFolder(const QString &folderPath);

	[[nodiscard]] QString folderPath() const {
		return _folderPath;
	}
	[[nodiscard]] QString mediaDirPath() const;

	// Ensures the folder / media subfolder / messages.md exist.
	bool ensureReady() const;

	// Parses messages.md. Returns notes in on-disk (chronological) order.
	[[nodiscard]] std::vector<Note> load() const;

	// Appends a new note and persists. Generates an id if note.id is empty;
	// the id actually stored is returned.
	QString append(Note note);

	// Replaces the note with the same id (matched by id) and persists.
	// Returns false if no note with that id exists.
	bool update(const Note &note);

	// Removes the note with the given id and persists.
	bool remove(const QString &id);

	// Copies an external file into media/ under a collision-free name and
	// returns its path relative to the folder (e.g. "media/photo_ab12.jpg"),
	// suitable for Note::media. Returns empty on failure.
	[[nodiscard]] QString importMedia(const QString &sourcePath) const;

	// Writes raw bytes (e.g. a pasted image) into media/ under a name derived
	// from the content hash + filenameHint's extension. Returns the relative
	// path, or empty on failure.
	[[nodiscard]] QString saveMediaBytes(
		const QByteArray &bytes,
		const QString &filenameHint) const;

	// Absolute path for a note media entry (folder + relative path).
	[[nodiscard]] QString mediaAbsolutePath(const QString &relative) const;

	// Generates a stable, sortable, unique note id.
	[[nodiscard]] static QString generateId();

	// Serializes / parses the whole note list. Exposed for testing and reuse.
	[[nodiscard]] static QString serialize(const std::vector<Note> &notes);
	[[nodiscard]] static std::vector<Note> parse(const QString &markdown);

private:
	bool saveAll(const std::vector<Note> &notes) const;

	[[nodiscard]] QString messagesFilePath() const;

	QString _folderPath;

};

} // namespace Storage::Offline
