/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"
#include "data/data_types.h"

namespace LocalAi {

// Fake peer identifiers. Real Telegram user ids are far below this range,
// so the local-only peers can never collide with a cached real account.
constexpr auto kSelfUserId = BareId(0x7A1000000001ULL);
constexpr auto kModelUserIdBase = BareId(0x7A1100000000ULL);

constexpr auto kDefaultServer = "http://127.0.0.1:8080";
constexpr auto kDefaultSystemPrompt = "You are a helpful assistant running "
	"locally on the user's machine. Answer concisely and use Markdown for "
	"code blocks.";

struct ModelEntry {
	QString id; // The id llama.cpp reports in /v1/models.
	QString title; // What we show as the chat title.
	BareId userId = 0; // Stable fake peer id, assigned once.
	QString systemPrompt; // Empty means "use the global one".
	bool removed = false; // Chat deleted by the user, keep the id mapping.

	[[nodiscard]] bool valid() const {
		return !id.isEmpty() && userId != 0;
	}
};

struct Config {
	bool enabled = true;
	QString server = QString::fromUtf8(kDefaultServer);
	QString apiKey;
	QString systemPrompt = QString::fromUtf8(kDefaultSystemPrompt);
	float64 temperature = 0.7;
	float64 topP = 0.95;
	int maxTokens = 2048;
	int contextMessages = 24;
	QString selfName = "You";
	std::vector<ModelEntry> models;
	BareId nextUserId = kModelUserIdBase;
	int64 nextMessageId = 1;
};

// The whole application talks to a single local server, so a single
// process-wide configuration is enough, same as Core::App().settings().
[[nodiscard]] Config &Current();
void Save();
void ScheduleSave();

[[nodiscard]] bool Enabled();

// tdata/local_ai/
[[nodiscard]] QString DataPath();

[[nodiscard]] ModelEntry *FindModelByUserId(BareId userId);
[[nodiscard]] ModelEntry *FindModelById(const QString &id);

// Creates the entry with a fresh stable userId if it is not known yet.
[[nodiscard]] not_null<ModelEntry*> EnsureModel(const QString &id);

// "qwen2.5-0.5b-instruct-q4_k_m.gguf" -> "Qwen2.5 0.5B Instruct"
[[nodiscard]] QString PrettyModelTitle(const QString &id);

// Server-like message ids must be unique across all local chats, because
// Data::Session keeps one non-channel message map for the whole account.
[[nodiscard]] MsgId TakeMessageId();

// Reserves a range at startup, so a crash can never re-use an id that a
// restored transcript already occupies.
void NoteUsedMessageId(MsgId id);

} // namespace LocalAi
