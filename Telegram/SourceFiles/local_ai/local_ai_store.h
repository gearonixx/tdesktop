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

struct StoredMessage {
	MsgId id = 0;
	bool out = false;
	TimeId date = 0;
	MsgId replyToId = 0;
	QString text;
};

// Transcripts live in tdata/local_ai/<userId>.jsonl, one message per line.
[[nodiscard]] std::vector<StoredMessage> ReadTranscript(BareId userId);
void AppendMessage(BareId userId, const StoredMessage &message);
void ReplaceMessage(BareId userId, const StoredMessage &message);
void RemoveMessages(BareId userId, const std::vector<MsgId> &ids);
void ClearTranscript(BareId userId);

} // namespace LocalAi
