/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_config.h"

#include "base/call_delayed.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace LocalAi {
namespace {

constexpr auto kSaveDelay = crl::time(1000);

Config GlobalConfig;
bool GlobalLoaded/* = false*/;
bool GlobalSaveScheduled/* = false*/;

[[nodiscard]] QString ConfigPath() {
	return DataPath() + u"config.json"_q;
}

[[nodiscard]] QString ReadString(
		const QJsonObject &object,
		const QString &key,
		const QString &fallback) {
	const auto value = object.value(key);
	return value.isString() ? value.toString() : fallback;
}

void ReadConfig() {
	auto file = QFile(ConfigPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Local AI Error: Bad config json: %1").arg(error.errorString()));
		return;
	}
	const auto root = document.object();
	auto &config = GlobalConfig;
	config.enabled = root.value(u"enabled"_q).toBool(config.enabled);
	config.server = ReadString(root, u"server"_q, config.server);
	config.apiKey = ReadString(root, u"apiKey"_q, config.apiKey);
	config.systemPrompt = ReadString(
		root,
		u"systemPrompt"_q,
		config.systemPrompt);
	config.temperature = root.value(u"temperature"_q).toDouble(
		config.temperature);
	config.topP = root.value(u"topP"_q).toDouble(config.topP);
	config.maxTokens = root.value(u"maxTokens"_q).toInt(config.maxTokens);
	config.contextMessages = root.value(u"contextMessages"_q).toInt(
		config.contextMessages);
	config.selfName = ReadString(root, u"selfName"_q, config.selfName);
	config.nextUserId = BareId(root.value(u"nextUserId"_q).toDouble(
		double(config.nextUserId)));
	if (config.nextUserId < kModelUserIdBase) {
		config.nextUserId = kModelUserIdBase;
	}
	config.nextMessageId = int64(root.value(u"nextMessageId"_q).toDouble(
		double(config.nextMessageId)));
	if (config.nextMessageId < 1) {
		config.nextMessageId = 1;
	}
	config.models.clear();
	const auto models = root.value(u"models"_q).toArray();
	for (const auto &modelValue : models) {
		const auto model = modelValue.toObject();
		auto entry = ModelEntry();
		entry.id = ReadString(model, u"id"_q, QString());
		entry.title = ReadString(model, u"title"_q, QString());
		entry.userId = BareId(model.value(u"userId"_q).toDouble(0.));
		entry.systemPrompt = ReadString(model, u"systemPrompt"_q, QString());
		entry.removed = model.value(u"removed"_q).toBool(false);
		if (entry.title.isEmpty()) {
			entry.title = PrettyModelTitle(entry.id);
		}
		if (entry.valid()) {
			config.models.push_back(std::move(entry));
		}
	}
}

void EnsureLoaded() {
	if (GlobalLoaded) {
		return;
	}
	GlobalLoaded = true;
	QDir().mkpath(DataPath());
	ReadConfig();
}

} // namespace

QString DataPath() {
	return cWorkingDir() + u"tdata/local_ai/"_q;
}

Config &Current() {
	EnsureLoaded();
	return GlobalConfig;
}

void Save() {
	EnsureLoaded();
	GlobalSaveScheduled = false;

	const auto &config = GlobalConfig;
	auto models = QJsonArray();
	for (const auto &model : config.models) {
		auto object = QJsonObject();
		object.insert(u"id"_q, model.id);
		object.insert(u"title"_q, model.title);
		object.insert(u"userId"_q, double(model.userId));
		if (!model.systemPrompt.isEmpty()) {
			object.insert(u"systemPrompt"_q, model.systemPrompt);
		}
		if (model.removed) {
			object.insert(u"removed"_q, true);
		}
		models.append(object);
	}
	auto root = QJsonObject();
	root.insert(u"enabled"_q, config.enabled);
	root.insert(u"server"_q, config.server);
	root.insert(u"apiKey"_q, config.apiKey);
	root.insert(u"systemPrompt"_q, config.systemPrompt);
	root.insert(u"temperature"_q, config.temperature);
	root.insert(u"topP"_q, config.topP);
	root.insert(u"maxTokens"_q, config.maxTokens);
	root.insert(u"contextMessages"_q, config.contextMessages);
	root.insert(u"selfName"_q, config.selfName);
	root.insert(u"nextUserId"_q, double(config.nextUserId));
	root.insert(u"nextMessageId"_q, double(config.nextMessageId));
	root.insert(u"models"_q, models);

	QDir().mkpath(DataPath());
	auto file = QFile(ConfigPath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Local AI Error: Can't write %1").arg(ConfigPath()));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void ScheduleSave() {
	EnsureLoaded();
	if (GlobalSaveScheduled) {
		return;
	}
	GlobalSaveScheduled = true;
	base::call_delayed(kSaveDelay, [] {
		if (GlobalSaveScheduled) {
			Save();
		}
	});
}

bool Enabled() {
	return Current().enabled;
}

ModelEntry *FindModelByUserId(BareId userId) {
	auto &config = Current();
	for (auto &model : config.models) {
		if (model.userId == userId) {
			return &model;
		}
	}
	return nullptr;
}

ModelEntry *FindModelById(const QString &id) {
	auto &config = Current();
	for (auto &model : config.models) {
		if (model.id == id) {
			return &model;
		}
	}
	return nullptr;
}

not_null<ModelEntry*> EnsureModel(const QString &id) {
	Expects(!id.isEmpty());

	if (const auto existing = FindModelById(id)) {
		return existing;
	}
	auto &config = Current();
	auto entry = ModelEntry();
	entry.id = id;
	entry.title = PrettyModelTitle(id);
	entry.userId = config.nextUserId++;
	config.models.push_back(std::move(entry));
	Save();
	return &config.models.back();
}

QString PrettyModelTitle(const QString &id) {
	auto name = id;
	const auto lastSlash = std::max(
		name.lastIndexOf(QChar('/')),
		name.lastIndexOf(QChar('\\')));
	if (lastSlash >= 0) {
		name = name.mid(lastSlash + 1);
	}
	for (const auto &suffix : { u".gguf"_q, u".bin"_q, u".safetensors"_q }) {
		if (name.endsWith(suffix, Qt::CaseInsensitive)) {
			name = name.left(name.size() - suffix.size());
		}
	}
	// Drop the quantization tail, it is noise in a chat list.
	static const auto kQuantization = QRegularExpression(
		u"[-_.](q[0-9]+[_a-z0-9]*|f16|f32|bf16|iq[0-9]+[_a-z0-9]*)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	while (true) {
		const auto match = kQuantization.match(name);
		if (!match.hasMatch()) {
			break;
		}
		name = name.left(match.capturedStart());
	}
	name.replace(QChar('-'), QChar(' '));
	name.replace(QChar('_'), QChar(' '));
	auto words = name.split(QChar(' '), Qt::SkipEmptyParts);
	for (auto &word : words) {
		if (word.isEmpty()) {
			continue;
		}
		// "0.5b" -> "0.5B", "7b" -> "7B", "instruct" -> "Instruct".
		static const auto kSize = QRegularExpression(
			u"^[0-9]+(\\.[0-9]+)?b$"_q,
			QRegularExpression::CaseInsensitiveOption);
		if (kSize.match(word).hasMatch()) {
			word = word.left(word.size() - 1) + QChar('B');
		} else {
			word[0] = word[0].toUpper();
		}
	}
	const auto result = words.join(QChar(' ')).trimmed();
	return result.isEmpty() ? id : result;
}

MsgId TakeMessageId() {
	auto &config = Current();
	const auto result = MsgId(config.nextMessageId++);
	ScheduleSave();
	return result;
}

void NoteUsedMessageId(MsgId id) {
	auto &config = Current();
	if (config.nextMessageId <= id.bare) {
		config.nextMessageId = id.bare + 1;
		ScheduleSave();
	}
}

} // namespace LocalAi
