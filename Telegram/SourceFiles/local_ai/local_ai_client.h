/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QPointer>

class QNetworkAccessManager;
class QNetworkReply;

namespace LocalAi {

struct ChatMessage {
	QString role; // "system" / "user" / "assistant"
	QString content;
};

struct GenerationRequest {
	QString model;
	std::vector<ChatMessage> messages;
	float64 temperature = 0.7;
	float64 topP = 0.95;
	int maxTokens = 0; // 0 keeps the server default.
};

struct StreamHandlers {
	// Called for every token chunk the server flushes.
	Fn<void(QString delta)> delta;
	// Called once, after the last delta.
	Fn<void()> done;
	// Called instead of `done` if anything went wrong.
	Fn<void(QString error)> fail;
};

struct ServerInfo {
	std::vector<QString> models;
};

// One in-flight completion. Destroying or cancelling it aborts the request
// and guarantees no further handler calls.
class Stream final : public base::has_weak_ptr {
public:
	Stream(QNetworkReply *reply, StreamHandlers handlers);
	Stream(const Stream &other) = delete;
	Stream &operator=(const Stream &other) = delete;
	~Stream();

	void cancel();
	[[nodiscard]] bool finished() const;

	// Everything received so far, deltas concatenated.
	[[nodiscard]] const QString &accumulated() const;

private:
	void setup();
	void handleData();
	void handleFinished();
	void feed(const QByteArray &line);
	void reportFail(const QString &error);

	QPointer<QNetworkReply> _reply;
	StreamHandlers _handlers;
	QByteArray _buffer;
	QString _accumulated;
	bool _finished = false;
	bool _cancelled = false;
	bool _sawDone = false;

};

class Client final : public base::has_weak_ptr {
public:
	Client();
	~Client();

	// GET {server}/v1/models
	void requestServerInfo(
		Fn<void(ServerInfo)> done,
		Fn<void(QString)> fail);

	// POST {server}/v1/chat/completions with stream: true
	[[nodiscard]] std::shared_ptr<Stream> start(
		GenerationRequest request,
		StreamHandlers handlers);

	// Reflects the currently configured server, resolved once per call.
	[[nodiscard]] QString endpoint(const QString &path) const;

private:
	const std::unique_ptr<QNetworkAccessManager> _manager;

};

} // namespace LocalAi
