/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "data/data_types.h"
#include "local_ai/local_ai_client.h"

class History;
class HistoryItem;
class PeerData;
class UserData;

namespace Api {
struct MessageToSend;
} // namespace Api

namespace Main {
class Session;
} // namespace Main

namespace LocalAi {

struct ModelEntry;

// Replaces the whole "talk to other people" side of the app with "talk to
// the models served by a local llama.cpp". Every chat in the dialogs list is
// one model; nothing here ever touches the network beyond that server.
class Chats final : public base::has_weak_ptr {
public:
	explicit Chats(not_null<Main::Session*> session);
	Chats(const Chats &other) = delete;
	Chats &operator=(const Chats &other) = delete;
	~Chats();

	// Creates the model peers and replays the stored transcripts. Safe to
	// call more than once, later calls only pick up newly known models.
	void bootstrap();

	[[nodiscard]] bool isModel(not_null<PeerData*> peer) const;
	[[nodiscard]] UserData *peerForModel(const QString &modelId);
	[[nodiscard]] QString modelIdFor(not_null<PeerData*> peer) const;

	// The single entry point ApiWrap::sendMessage() defers to.
	[[nodiscard]] bool trySend(const Api::MessageToSend &message);

	[[nodiscard]] bool generating(not_null<History*> history) const;
	void stop(not_null<History*> history);
	void regenerate(not_null<History*> history);
	void clearHistory(not_null<History*> history);
	void deleteChat(not_null<History*> history);

	// GET /v1/models, then create a chat for every model we did not see yet.
	void refreshModels(
		Fn<void()> done = nullptr,
		Fn<void(QString)> fail = nullptr);

	[[nodiscard]] rpl::producer<> modelsUpdated() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	[[nodiscard]] Client &client() const;

private:
	struct Generation {
		std::shared_ptr<Stream> stream;
		FullMsgId itemId;
		QString text;
		bool textDirty = false;
		bool stopped = false;
		crl::time started = 0;
	};

	UserData *ensurePeer(ModelEntry &model);
	void restore(not_null<History*> history, BareId userId);
	void greet(not_null<History*> history, const ModelEntry &model);
	[[nodiscard]] TextWithEntities loadingEmoji();
	void applyStreamText(
		not_null<HistoryItem*> item,
		const QString &text,
		bool complete);

	not_null<HistoryItem*> addMessage(
		not_null<History*> history,
		bool out,
		const TextWithEntities &text,
		MsgId replyToId,
		bool streaming = false);

	void generate(not_null<History*> history, MsgId replyToId);
	void applyPendingText();
	void finish(not_null<History*> history, const QString &error);
	void refreshTyping();

	[[nodiscard]] std::vector<ChatMessage> collectContext(
		not_null<History*> history) const;

	const not_null<Main::Session*> _session;
	const std::unique_ptr<Client> _client;

	base::flat_map<not_null<History*>, Generation> _generations;
	base::Timer _updateTimer;
	base::Timer _typingTimer;
	TextWithEntities _loadingEmoji;

	rpl::event_stream<> _modelsUpdated;
	rpl::event_stream<QString> _errors;
	bool _refreshing = false;

	rpl::lifetime _lifetime;

};

} // namespace LocalAi
