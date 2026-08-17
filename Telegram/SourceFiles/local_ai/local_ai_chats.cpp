/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_chats.h"

#include "api/api_common.h"
#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "chat_helpers/stickers_lottie.h"
#include "data/data_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "local_ai/local_ai_config.h"
#include "local_ai/local_ai_store.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "ui/text/text_utilities.h"

namespace LocalAi {
namespace {

// llama.cpp streams token by token, re-laying out the bubble that often is
// wasteful, so the text is applied on a timer instead.
constexpr auto kTextUpdateInterval = crl::time(60);
constexpr auto kTypingRefreshInterval = crl::time(4000);

constexpr auto kMarkdownFlags = TextParseLinks
	| TextParseMultiline
	| TextParseMarkdown;

[[nodiscard]] MTPUser MakeModelUser(const ModelEntry &model) {
	using Flag = MTPDuser::Flag;
	const auto flags = Flag::f_first_name
		| Flag::f_bot // Shares the bit with bot_info_version.
		| Flag::f_bot_chat_history
		| Flag::f_bot_nochats;
	return MTP_user(
		MTP_flags(flags),
		MTP_long(model.userId),
		MTPlong(), // access_hash
		MTP_string(model.title),
		MTPstring(), // last_name
		MTPstring(), // username
		MTPstring(), // phone
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTP_int(1), // bot_info_version
		MTPVector<MTPRestrictionReason>(),
		MTPstring(), // bot_inline_placeholder
		MTPstring(), // lang_code
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(), // color
		MTPPeerColor(), // profile_color
		MTPint(), // bot_active_users
		MTPlong(), // bot_verification_icon
		MTPlong(), // send_paid_messages_stars
		MTPlong()); // linked_community_id
}

[[nodiscard]] MTPMessage MakeMessage(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId id,
		bool out,
		TimeId date,
		const TextWithEntities &text,
		MsgId replyToId) {
	using Flag = MTPDmessage::Flag;
	auto entities = Api::EntitiesToMTP(
		session,
		text.entities,
		Api::ConvertOption::SkipLocal);
	auto flags = Flag::f_from_id
		| (out ? Flag::f_out : Flag())
		| (entities.v.isEmpty() ? Flag() : Flag::f_entities)
		| (replyToId ? Flag::f_reply_to : Flag());
	const auto fromId = out ? session->userPeerId() : peer->id;
	using ReplyFlag = MTPDmessageReplyHeader::Flag;
	return MTP_message(
		MTP_flags(flags),
		MTP_int(int32(id.bare)),
		peerToMTP(fromId),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(peer->id),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(), // fwd_from
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		(replyToId
			? MTP_messageReplyHeader(
				MTP_flags(ReplyFlag::f_reply_to_msg_id),
				MTP_int(int32(replyToId.bare)),
				MTPPeer(), // reply_to_peer_id
				MTPMessageFwdHeader(), // reply_from
				MTPMessageMedia(), // reply_media
				MTPint(), // reply_to_top_id
				MTPstring(), // quote_text
				MTPVector<MTPMessageEntity>(), // quote_entities
				MTPint(), // quote_offset
				MTPint(), // todo_item_id
				MTPbytes()) // poll_option
			: MTPMessageReplyHeader()),
		MTP_int(date),
		MTP_string(text.text),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		std::move(entities),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTPstring(), // post_author
		MTPlong(), // grouped_id
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

// Markdown from a model is applied while it streams, but a fenced block with
// a language would kick off one syntax-highlight job per token, so the
// language is dropped until the answer is complete.
[[nodiscard]] TextWithEntities ParseMarkdown(QString source, bool complete) {
	auto result = TextWithEntities{ std::move(source) };
	TextUtilities::ParseEntities(result, kMarkdownFlags);
	if (!complete) {
		for (auto &entity : result.entities) {
			if (entity.type() == EntityType::Pre && !entity.data().isEmpty()) {
				entity = EntityInText(
					EntityType::Pre,
					entity.offset(),
					entity.length());
			}
		}
	}
	return result;
}

} // namespace

Chats::Chats(not_null<Main::Session*> session)
: _session(session)
, _client(std::make_unique<Client>())
, _updateTimer([=] { applyPendingText(); })
, _typingTimer([=] { refreshTyping(); }) {
}

Chats::~Chats() {
	for (auto &[history, generation] : _generations) {
		if (generation.stream) {
			generation.stream->cancel();
		}
	}
}

Client &Chats::client() const {
	return *_client;
}

rpl::producer<> Chats::modelsUpdated() const {
	return _modelsUpdated.events();
}

rpl::producer<QString> Chats::errors() const {
	return _errors.events();
}

void Chats::bootstrap() {
	if (!Enabled()) {
		return;
	}
	auto &config = Current();
	for (auto &model : config.models) {
		if (!model.removed) {
			ensurePeer(model);
		}
	}
	// Nothing will ever arrive from a server, so the dialogs list must be
	// declared complete or it keeps showing the loading state.
	_session->data().chatsListDone(nullptr);
	refreshModels();
}

UserData *Chats::ensurePeer(ModelEntry &model) {
	if (!model.valid()) {
		return nullptr;
	}
	const auto user = _session->data().processUser(MakeModelUser(model));
	const auto history = _session->data().history(user);
	if (!history->folderKnown()) {
		// Makes History::shouldBeInChatList() answer honestly, which is what
		// puts the chat into the dialogs list.
		history->clearFolder();
	}
	if (history->isEmpty()) {
		restore(history, model.userId);
		if (history->isEmpty()) {
			greet(history, model);
		}
	}
	return user;
}

void Chats::restore(not_null<History*> history, BareId userId) {
	const auto stored = ReadTranscript(userId);
	if (stored.empty()) {
		history->addOlderSlice({});
		return;
	}
	auto slice = QVector<MTPMessage>();
	slice.reserve(int(stored.size()));
	// History::createItems() walks its argument backwards, the same way the
	// server hands out history pages: newest first.
	for (auto i = stored.crbegin(); i != stored.crend(); ++i) {
		NoteUsedMessageId(i->id);
		slice.push_back(MakeMessage(
			_session,
			history->peer,
			i->id,
			i->out,
			i->date,
			ParseMarkdown(i->text, true),
			i->replyToId));
	}
	history->addOlderSlice(slice);
	history->addOlderSlice({}); // Marks the history as loaded to the top.
	history->inboxRead(stored.back().id, 0);
}

void Chats::greet(not_null<History*> history, const ModelEntry &model) {
	const auto text = u"**%1** is served from `%2`.\n\nSend a message to "
		"start a conversation. Nothing here goes through Telegram."_q
		.arg(model.title)
		.arg(Current().server);
	// Goes through the store so that it is restored like any other message,
	// and so that it never counts as unread or raises a notification.
	AppendMessage(model.userId, {
		.id = TakeMessageId(),
		.out = false,
		.date = base::unixtime::now(),
		.text = text,
	});
	restore(history, model.userId);
}

TextWithEntities Chats::loadingEmoji() {
	if (_loadingEmoji.empty()) {
		_loadingEmoji = Data::SingleCustomEmoji(
			ChatHelpers::GenerateLocalTgsSticker(
				_session,
				u"transcribe_loading"_q,
				true));
	}
	return _loadingEmoji;
}

void Chats::applyStreamText(
		not_null<HistoryItem*> item,
		const QString &text,
		bool complete) {
	auto parsed = ParseMarkdown(text, complete);
	if (!complete) {
		parsed.append(loadingEmoji());
	}
	item->setText(std::move(parsed));
	_session->data().requestItemTextRefresh(item);
	item->invalidateChatListEntry();
}

bool Chats::isModel(not_null<PeerData*> peer) const {
	const auto user = peer->asUser();
	return user && (FindModelByUserId(peerToUser(user->id).bare) != nullptr);
}

QString Chats::modelIdFor(not_null<PeerData*> peer) const {
	const auto user = peer->asUser();
	if (!user) {
		return QString();
	}
	const auto model = FindModelByUserId(peerToUser(user->id).bare);
	return model ? model->id : QString();
}

UserData *Chats::peerForModel(const QString &modelId) {
	const auto model = FindModelById(modelId);
	return model ? ensurePeer(*model) : nullptr;
}

not_null<HistoryItem*> Chats::addMessage(
		not_null<History*> history,
		bool out,
		const TextWithEntities &text,
		MsgId replyToId,
		bool streaming) {
	const auto id = TakeMessageId();
	const auto date = base::unixtime::now();
	if (!out) {
		// Marking the id read before the item exists keeps the answer out of
		// the unread counter and stops a notification from firing for the
		// placeholder that is about to be filled in token by token.
		history->inboxRead(id, 0);
	}
	const auto item = history->addNewMessage(
		id,
		MakeMessage(_session, history->peer, id, out, date, text, replyToId),
		(streaming
			? MessageFlags(MessageFlag::TextAppearing)
			: MessageFlags()),
		NewMessageType::Unread);
	if (!streaming) {
		const auto userId = peerToUser(history->peer->id).bare;
		AppendMessage(userId, {
			.id = id,
			.out = out,
			.date = date,
			.replyToId = replyToId,
			.text = text.text,
		});
	}
	return item;
}

bool Chats::trySend(const Api::MessageToSend &message) {
	if (!Enabled()) {
		return false;
	}
	const auto history = message.action.history;
	if (!isModel(history->peer)) {
		return false;
	} else if (generating(history)) {
		_errors.fire(u"The model is still answering, stop it first."_q);
		return true;
	}
	auto text = TextWithEntities{
		message.textWithTags.text,
		TextUtilities::ConvertTextTagsToEntities(message.textWithTags.tags),
	};
	TextUtilities::Trim(text);
	if (text.empty()) {
		return true;
	}
	const auto replyToId = message.action.replyTo.messageId.msg;
	addMessage(history, true, text, replyToId);
	generate(history, replyToId);
	return true;
}

std::vector<ChatMessage> Chats::collectContext(
		not_null<History*> history) const {
	const auto model = FindModelByUserId(peerToUser(history->peer->id).bare);
	auto result = std::vector<ChatMessage>();
	const auto &config = Current();
	const auto prompt = (model && !model->systemPrompt.isEmpty())
		? model->systemPrompt
		: config.systemPrompt;
	if (!prompt.isEmpty()) {
		result.push_back({ .role = u"system"_q, .content = prompt });
	}
	auto stored = ReadTranscript(peerToUser(history->peer->id).bare);
	const auto limit = std::max(config.contextMessages, 2);
	if (int(stored.size()) > limit) {
		stored.erase(begin(stored), end(stored) - limit);
	}
	for (const auto &message : stored) {
		if (message.text.isEmpty()) {
			continue;
		}
		result.push_back({
			.role = message.out ? u"user"_q : u"assistant"_q,
			.content = message.text,
		});
	}
	return result;
}

void Chats::generate(not_null<History*> history, MsgId replyToId) {
	const auto model = FindModelByUserId(peerToUser(history->peer->id).bare);
	if (!model) {
		return;
	}
	// Created with a placeholder and then filled in via setText(), so the
	// spinner never has to survive a round trip through MTP entities.
	const auto item = addMessage(
		history,
		false,
		TextWithEntities{ QString::fromUtf8("\xE2\x80\xA6") },
		replyToId,
		true);
	const auto itemId = item->fullId();
	applyStreamText(item, QString(), false);

	const auto &config = Current();
	auto request = GenerationRequest{
		.model = model->id,
		.messages = collectContext(history),
		.temperature = config.temperature,
		.topP = config.topP,
		.maxTokens = config.maxTokens,
	};

	_generations[history] = Generation{
		.itemId = itemId,
		.started = crl::now(),
	};

	// The request may fail before start() even returns, which erases the
	// entry again, so the handle is stored only after we know it is alive.
	const auto weak = base::make_weak(this);
	auto stream = _client->start(std::move(request), {
		.delta = [=](QString delta) {
			if (const auto strong = weak.get()) {
				const auto i = strong->_generations.find(history);
				if (i != end(strong->_generations)) {
					i->second.text += delta;
					i->second.textDirty = true;
					if (!strong->_updateTimer.isActive()) {
						strong->_updateTimer.callOnce(kTextUpdateInterval);
					}
				}
			}
		},
		.done = [=] {
			if (const auto strong = weak.get()) {
				strong->finish(history, QString());
			}
		},
		.fail = [=](QString error) {
			if (const auto strong = weak.get()) {
				strong->finish(history, error);
			}
		},
	});
	if (const auto i = _generations.find(history); i != end(_generations)) {
		i->second.stream = std::move(stream);
	}

	crl::on_main(this, [=] {
		crl::on_main(this, [=] {
			// Views appear one main-loop hop after the item, the typewriter
			// reveal has to be started once there is something to reveal.
			if (const auto item = _session->data().message(itemId)) {
				item->markTextAppearingStarted();
			}
		});
	});

	refreshTyping();
	_typingTimer.callEach(kTypingRefreshInterval);
}

void Chats::applyPendingText() {
	auto anyDirty = false;
	for (auto &[history, generation] : _generations) {
		if (!generation.textDirty) {
			continue;
		}
		generation.textDirty = false;
		const auto item = _session->data().message(generation.itemId);
		if (!item) {
			continue;
		}
		applyStreamText(item, generation.text, false);
		anyDirty = true;
	}
	if (anyDirty) {
		_updateTimer.callOnce(kTextUpdateInterval);
	}
}

void Chats::finish(not_null<History*> history, const QString &error) {
	const auto i = _generations.find(history);
	if (i == end(_generations)) {
		return;
	}
	auto generation = std::move(i->second);
	_generations.erase(i);
	if (_generations.empty()) {
		_typingTimer.cancel();
		_updateTimer.cancel();
	}
	if (auto stream = std::move(generation.stream)) {
		stream->cancel();
		// finish() runs from inside the stream's own callback, the handle
		// must outlive the current stack frame.
		crl::on_main([stream = std::move(stream)]() mutable {
			stream = nullptr;
		});
	}

	const auto item = _session->data().message(generation.itemId);
	auto body = generation.text.trimmed();
	if (!error.isEmpty()) {
		const auto note = u"⚠️ %1"_q.arg(error);
		body = body.isEmpty() ? note : (body + u"\n\n"_q + note);
		_errors.fire_copy(error);
	} else if (generation.stopped && body.isEmpty()) {
		body = u"_(stopped)_"_q;
	}
	if (body.isEmpty()) {
		body = u"_(empty answer)_"_q;
	}
	if (!item) {
		return;
	}
	applyStreamText(item, body, true);

	AppendMessage(peerToUser(history->peer->id).bare, {
		.id = item->id,
		.out = false,
		.date = item->date(),
		.replyToId = item->replyToId(),
		.text = body,
	});
}

bool Chats::generating(not_null<History*> history) const {
	return _generations.contains(history);
}

void Chats::stop(not_null<History*> history) {
	const auto i = _generations.find(history);
	if (i == end(_generations)) {
		return;
	}
	i->second.stopped = true;
	if (i->second.stream) {
		i->second.stream->cancel();
	}
	finish(history, QString());
}

void Chats::regenerate(not_null<History*> history) {
	if (generating(history)) {
		stop(history);
	}
	const auto userId = peerToUser(history->peer->id).bare;
	auto stored = ReadTranscript(userId);
	// Drop trailing model answers, then re-ask with the same user message.
	auto removed = std::vector<MsgId>();
	while (!stored.empty() && !stored.back().out) {
		removed.push_back(stored.back().id);
		stored.pop_back();
	}
	if (stored.empty()) {
		return;
	}
	RemoveMessages(userId, removed);
	for (const auto id : removed) {
		if (const auto item = _session->data().message(history->peer->id, id)) {
			item->destroy();
		}
	}
	generate(history, MsgId());
}

void Chats::clearHistory(not_null<History*> history) {
	if (generating(history)) {
		stop(history);
	}
	const auto userId = peerToUser(history->peer->id).bare;
	ClearTranscript(userId);
	// DeleteChat is the variant that really destroys every item; ClearHistory
	// would keep the last one around as an emptied stub.
	history->clear(History::ClearType::DeleteChat);
	if (const auto model = FindModelByUserId(userId)) {
		// Puts the chat back into the list with a fresh greeting.
		greet(history, *model);
	}
}

void Chats::deleteChat(not_null<History*> history) {
	if (generating(history)) {
		stop(history);
	}
	const auto userId = peerToUser(history->peer->id).bare;
	ClearTranscript(userId);
	// The id mapping is kept, so a re-added model reuses the same peer.
	if (const auto model = FindModelByUserId(userId)) {
		model->removed = true;
		Save();
	}
	history->clear(History::ClearType::DeleteChat);
	_modelsUpdated.fire({});
}

void Chats::refreshTyping() {
	if (_generations.empty()) {
		_typingTimer.cancel();
		return;
	}
	const auto when = base::unixtime::now();
	for (const auto &[history, generation] : _generations) {
		if (const auto user = history->peer->asUser()) {
			_session->data().sendActionManager().registerFor(
				history,
				MsgId(0),
				user,
				MTP_sendMessageTypingAction(),
				when);
		}
	}
}

void Chats::refreshModels(Fn<void()> done, Fn<void(QString)> fail) {
	if (_refreshing) {
		return;
	}
	_refreshing = true;
	const auto weak = base::make_weak(this);
	_client->requestServerInfo([=](ServerInfo info) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		strong->_refreshing = false;
		auto added = false;
		for (const auto &id : info.models) {
			const auto known = FindModelById(id);
			if (known && !known->removed) {
				continue;
			}
			const auto model = EnsureModel(id);
			model->removed = false;
			Save();
			strong->ensurePeer(*model);
			added = true;
		}
		if (added) {
			strong->_session->data().chatsListDone(nullptr);
			strong->_modelsUpdated.fire({});
		}
		if (done) {
			done();
		}
	}, [=](QString error) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		strong->_refreshing = false;
		strong->_errors.fire_copy(error);
		if (fail) {
			fail(error);
		}
	});
}

} // namespace LocalAi
