/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/offline/offline_notes_storage.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtCore/QCryptographicHash>

namespace Storage::Offline {
namespace {

constexpr auto kMessagesFile = "messages.md";
constexpr auto kMediaDir = "media";
constexpr auto kNoteOpen = "<!--note ";
constexpr auto kNoteClose = "<!--/note-->";

// The literal end marker can never appear inside a body on disk, so escape it
// on write and restore on read to keep arbitrary text round-trippable.
const auto kCloseEscaped = QStringLiteral("<!--\\/note-->");

[[nodiscard]] QString escapeBody(QString body) {
	return body.replace(
		QLatin1String(kNoteClose),
		kCloseEscaped);
}

[[nodiscard]] QString unescapeBody(QString body) {
	return body.replace(
		kCloseEscaped,
		QLatin1String(kNoteClose));
}

[[nodiscard]] QString attrEscape(QString value) {
	return value
		.replace('\\', QStringLiteral("\\\\"))
		.replace('"', QStringLiteral("\\\""));
}

[[nodiscard]] QString attrUnescape(QString value) {
	return value
		.replace(QStringLiteral("\\\""), QStringLiteral("\""))
		.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
}

[[nodiscard]] QString toIso(qint64 unixSeconds) {
	if (!unixSeconds) {
		return QString();
	}
	return QDateTime::fromSecsSinceEpoch(unixSeconds, QTimeZone::UTC)
		.toString(Qt::ISODate);
}

[[nodiscard]] qint64 fromIso(const QString &iso) {
	if (iso.isEmpty()) {
		return 0;
	}
	const auto parsed = QDateTime::fromString(iso, Qt::ISODate);
	return parsed.isValid() ? parsed.toUTC().toSecsSinceEpoch() : 0;
}

[[nodiscard]] QString reactionsField(const std::vector<NoteReaction> &list) {
	QStringList parts;
	parts.reserve(int(list.size()));
	for (const auto &reaction : list) {
		if (reaction.emoji.isEmpty() || reaction.count <= 0) {
			continue;
		}
		parts.push_back(reaction.emoji + ':' + QString::number(reaction.count));
	}
	return parts.join(',');
}

[[nodiscard]] std::vector<NoteReaction> parseReactions(const QString &field) {
	std::vector<NoteReaction> result;
	if (field.isEmpty()) {
		return result;
	}
	for (const auto &part : field.split(',', Qt::SkipEmptyParts)) {
		const auto colon = part.lastIndexOf(':');
		if (colon <= 0) {
			continue;
		}
		auto reaction = NoteReaction();
		reaction.emoji = part.left(colon);
		reaction.count = part.mid(colon + 1).toInt();
		if (reaction.count <= 0) {
			reaction.count = 1;
		}
		result.push_back(std::move(reaction));
	}
	return result;
}

[[nodiscard]] QString mediaField(const std::vector<QString> &media) {
	QStringList parts;
	parts.reserve(int(media.size()));
	for (auto path : media) {
		// '|' separates entries, so percent-escape it (and '%') in paths.
		parts.push_back(path
			.replace('%', QStringLiteral("%25"))
			.replace('|', QStringLiteral("%7C")));
	}
	return parts.join('|');
}

[[nodiscard]] std::vector<QString> parseMedia(const QString &field) {
	std::vector<QString> result;
	if (field.isEmpty()) {
		return result;
	}
	for (auto part : field.split('|', Qt::SkipEmptyParts)) {
		result.push_back(part
			.replace(QStringLiteral("%7C"), QStringLiteral("|"))
			.replace(QStringLiteral("%25"), QStringLiteral("%")));
	}
	return result;
}

// Reads key="value" attributes from a note-open marker line.
[[nodiscard]] QString readAttr(const QString &line, const QString &key) {
	const auto pattern = key + QStringLiteral("=\"");
	const auto from = line.indexOf(pattern);
	if (from < 0) {
		return QString();
	}
	auto i = from + pattern.size();
	QString value;
	while (i < line.size()) {
		const auto ch = line[i];
		if (ch == '\\' && i + 1 < line.size()) {
			value.append(line[i + 1]);
			i += 2;
			continue;
		} else if (ch == '"') {
			break;
		}
		value.append(ch);
		++i;
	}
	return attrUnescape(value);
}

} // namespace

NotesFolder::NotesFolder(const QString &folderPath)
: _folderPath(folderPath) {
}

QString NotesFolder::messagesFilePath() const {
	return _folderPath + '/' + QLatin1String(kMessagesFile);
}

QString NotesFolder::mediaDirPath() const {
	return _folderPath + '/' + QLatin1String(kMediaDir);
}

bool NotesFolder::ensureReady() const {
	auto dir = QDir(_folderPath);
	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
		return false;
	}
	if (!QDir(mediaDirPath()).exists()
		&& !dir.mkpath(QLatin1String(kMediaDir))) {
		return false;
	}
	const auto path = messagesFilePath();
	if (!QFile::exists(path)) {
		auto file = QFile(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return false;
		}
	}
	return true;
}

QString NotesFolder::generateId() {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	static qint64 lastNow = 0;
	static int counter = 0;
	if (now == lastNow) {
		++counter;
	} else {
		lastNow = now;
		counter = 0;
	}
	return QStringLiteral("%1-%2")
		.arg(now)
		.arg(counter, 4, 10, QChar('0'));
}

QString NotesFolder::serialize(const std::vector<Note> &notes) {
	QString out;
	for (const auto &note : notes) {
		out += QLatin1String(kNoteOpen);
		out += QStringLiteral("id=\"%1\"").arg(attrEscape(note.id));
		out += QStringLiteral(" date=\"%1\"").arg(toIso(note.date));
		if (note.edited > 0) {
			out += QStringLiteral(" edited=\"%1\"").arg(toIso(note.edited));
		}
		if (note.pinned) {
			out += QStringLiteral(" pinned=\"1\"");
		}
		const auto reactions = reactionsField(note.reactions);
		if (!reactions.isEmpty()) {
			out += QStringLiteral(" reactions=\"%1\"").arg(attrEscape(reactions));
		}
		const auto media = mediaField(note.media);
		if (!media.isEmpty()) {
			out += QStringLiteral(" media=\"%1\"").arg(attrEscape(media));
		}
		out += QStringLiteral("-->\n");
		out += escapeBody(note.text);
		out += '\n';
		out += QLatin1String(kNoteClose);
		out += QStringLiteral("\n\n");
	}
	return out;
}

std::vector<Note> NotesFolder::parse(const QString &markdown) {
	std::vector<Note> result;
	const auto lines = markdown.split('\n');
	auto i = 0;
	const auto count = lines.size();
	while (i < count) {
		const auto &line = lines[i];
		if (!line.startsWith(QLatin1String(kNoteOpen))) {
			++i;
			continue;
		}
		auto note = Note();
		note.id = readAttr(line, QStringLiteral("id"));
		note.date = fromIso(readAttr(line, QStringLiteral("date")));
		note.edited = fromIso(readAttr(line, QStringLiteral("edited")));
		note.pinned = (readAttr(line, QStringLiteral("pinned")) == QLatin1String("1"));
		note.reactions = parseReactions(readAttr(line, QStringLiteral("reactions")));
		note.media = parseMedia(readAttr(line, QStringLiteral("media")));

		++i;
		QStringList body;
		while (i < count && lines[i] != QLatin1String(kNoteClose)) {
			body.push_back(lines[i]);
			++i;
		}
		if (i < count) {
			++i; // skip the close marker.
		}
		note.text = unescapeBody(body.join('\n'));
		if (note.id.isEmpty()) {
			note.id = generateId();
		}
		result.push_back(std::move(note));
	}
	return result;
}

std::vector<Note> NotesFolder::load() const {
	auto file = QFile(messagesFilePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}
	return parse(QString::fromUtf8(file.readAll()));
}

bool NotesFolder::saveAll(const std::vector<Note> &notes) const {
	if (!ensureReady()) {
		return false;
	}
	auto file = QSaveFile(messagesFilePath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	file.write(serialize(notes).toUtf8());
	return file.commit();
}

QString NotesFolder::append(Note note) {
	if (note.id.isEmpty()) {
		note.id = generateId();
	}
	if (!note.date) {
		note.date = QDateTime::currentSecsSinceEpoch();
	}
	auto notes = load();
	const auto id = note.id;
	notes.push_back(std::move(note));
	return saveAll(notes) ? id : QString();
}

bool NotesFolder::update(const Note &note) {
	auto notes = load();
	for (auto &existing : notes) {
		if (existing.id == note.id) {
			existing = note;
			return saveAll(notes);
		}
	}
	return false;
}

bool NotesFolder::remove(const QString &id) {
	auto notes = load();
	const auto before = notes.size();
	notes.erase(
		std::remove_if(
			notes.begin(),
			notes.end(),
			[&](const Note &note) { return note.id == id; }),
		notes.end());
	if (notes.size() == before) {
		return false;
	}
	return saveAll(notes);
}

QString NotesFolder::importMedia(const QString &sourcePath) const {
	const auto source = QFileInfo(sourcePath);
	if (!source.exists() || !source.isFile()) {
		return QString();
	}
	if (!ensureReady()) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha1);
	hash.addData(
		(source.absoluteFilePath()
			+ '|'
			+ QString::number(source.size())
			+ '|'
			+ QString::number(source.lastModified().toMSecsSinceEpoch()))
		.toUtf8());
	const auto suffix = source.suffix();
	const auto base = source.completeBaseName().left(40);
	const auto stamp = QString::fromLatin1(hash.result().toHex()).left(8);
	auto name = base + '_' + stamp + (suffix.isEmpty() ? QString() : '.' + suffix);
	auto target = mediaDirPath() + '/' + name;
	if (!QFile::exists(target)) {
		if (!QFile::copy(sourcePath, target)) {
			return QString();
		}
	}
	return QLatin1String(kMediaDir) + '/' + name;
}

QString NotesFolder::saveMediaBytes(
		const QByteArray &bytes,
		const QString &filenameHint) const {
	if (bytes.isEmpty() || !ensureReady()) {
		return QString();
	}
	const auto hash = QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex())
		.left(12);
	const auto info = QFileInfo(filenameHint);
	const auto suffix = info.suffix().isEmpty()
		? QStringLiteral("bin")
		: info.suffix();
	const auto base = info.completeBaseName().left(40);
	const auto name = (base.isEmpty() ? QStringLiteral("media") : base)
		+ '_' + hash + '.' + suffix;
	const auto target = mediaDirPath() + '/' + name;
	if (!QFile::exists(target)) {
		auto file = QFile(target);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			|| file.write(bytes) != bytes.size()) {
			return QString();
		}
	}
	return QLatin1String(kMediaDir) + '/' + name;
}

QString NotesFolder::mediaAbsolutePath(const QString &relative) const {
	return relative.isEmpty() ? QString() : (_folderPath + '/' + relative);
}

} // namespace Storage::Offline
