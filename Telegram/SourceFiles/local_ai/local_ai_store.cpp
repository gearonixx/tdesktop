/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_store.h"

#include "local_ai/local_ai_config.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

namespace LocalAi {
namespace {

[[nodiscard]] QString TranscriptPath(BareId userId) {
	return DataPath() + u"chat-%1.jsonl"_q.arg(userId);
}

[[nodiscard]] QByteArray Serialize(const StoredMessage &message) {
	auto object = QJsonObject();
	object.insert(u"id"_q, double(message.id.bare));
	object.insert(u"out"_q, message.out);
	object.insert(u"date"_q, double(message.date));
	if (message.replyToId) {
		object.insert(u"replyToId"_q, double(message.replyToId.bare));
	}
	object.insert(u"text"_q, message.text);
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<StoredMessage> Deserialize(
		const QByteArray &line) {
	if (line.trimmed().isEmpty()) {
		return std::nullopt;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(line, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto result = StoredMessage();
	result.id = MsgId(int64(object.value(u"id"_q).toDouble(0.)));
	result.out = object.value(u"out"_q).toBool(false);
	result.date = TimeId(object.value(u"date"_q).toDouble(0.));
	result.replyToId = MsgId(int64(object.value(u"replyToId"_q).toDouble(0.)));
	result.text = object.value(u"text"_q).toString();
	if (!result.id) {
		return std::nullopt;
	}
	return result;
}

void WriteAll(BareId userId, const std::vector<StoredMessage> &messages) {
	QDir().mkpath(DataPath());
	const auto path = TranscriptPath(userId);
	const auto temporary = path + u".tmp"_q;
	auto file = QFile(temporary);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Local AI Error: Can't write %1").arg(temporary));
		return;
	}
	for (const auto &message : messages) {
		file.write(Serialize(message));
		file.write("\n");
	}
	file.close();
	QFile::remove(path);
	if (!QFile::rename(temporary, path)) {
		LOG(("Local AI Error: Can't move %1 to %2"
			).arg(temporary).arg(path));
	}
}

} // namespace

std::vector<StoredMessage> ReadTranscript(BareId userId) {
	auto result = std::vector<StoredMessage>();
	auto file = QFile(TranscriptPath(userId));
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	while (!file.atEnd()) {
		if (auto message = Deserialize(file.readLine())) {
			result.push_back(std::move(*message));
		}
	}
	ranges::stable_sort(result, ranges::less(), &StoredMessage::id);
	return result;
}

void AppendMessage(BareId userId, const StoredMessage &message) {
	QDir().mkpath(DataPath());
	auto file = QFile(TranscriptPath(userId));
	if (!file.open(QIODevice::Append)) {
		LOG(("Local AI Error: Can't append to %1"
			).arg(TranscriptPath(userId)));
		return;
	}
	file.write(Serialize(message));
	file.write("\n");
}

void ReplaceMessage(BareId userId, const StoredMessage &message) {
	auto all = ReadTranscript(userId);
	const auto i = ranges::find(all, message.id, &StoredMessage::id);
	if (i == end(all)) {
		AppendMessage(userId, message);
		return;
	}
	*i = message;
	WriteAll(userId, all);
}

void RemoveMessages(BareId userId, const std::vector<MsgId> &ids) {
	if (ids.empty()) {
		return;
	}
	auto all = ReadTranscript(userId);
	const auto removed = std::remove_if(begin(all), end(all), [&](
			const StoredMessage &message) {
		return ranges::contains(ids, message.id);
	});
	if (removed == end(all)) {
		return;
	}
	all.erase(removed, end(all));
	WriteAll(userId, all);
}

void ClearTranscript(BareId userId) {
	QFile::remove(TranscriptPath(userId));
}

} // namespace LocalAi
