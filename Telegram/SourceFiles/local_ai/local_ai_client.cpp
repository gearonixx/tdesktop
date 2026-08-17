/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_client.h"

#include "local_ai/local_ai_config.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace LocalAi {
namespace {

constexpr auto kDataPrefix = "data:";
constexpr auto kDoneMarker = "[DONE]";

[[nodiscard]] QString NormalizedBase(QString server) {
	server = server.trimmed();
	if (server.isEmpty()) {
		server = QString::fromUtf8(kDefaultServer);
	}
	if (!server.contains(u"://"_q)) {
		server = u"http://"_q + server;
	}
	while (server.endsWith(QChar('/'))) {
		server.chop(1);
	}
	// Accept both "http://host:8080" and "http://host:8080/v1".
	if (server.endsWith(u"/v1"_q, Qt::CaseInsensitive)) {
		server.chop(3);
	}
	return server;
}

[[nodiscard]] QString ErrorFromBody(const QByteArray &body) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		const auto text = QString::fromUtf8(body).trimmed();
		return text.isEmpty() ? QString() : text;
	}
	const auto root = document.object();
	const auto value = root.value(u"error"_q);
	if (value.isString()) {
		return value.toString();
	} else if (value.isObject()) {
		const auto message = value.toObject().value(u"message"_q);
		if (message.isString()) {
			return message.toString();
		}
	}
	const auto message = root.value(u"message"_q);
	return message.isString() ? message.toString() : QString();
}

[[nodiscard]] QString DescribeReply(not_null<QNetworkReply*> reply) {
	const auto status = reply
		->attribute(QNetworkRequest::HttpStatusCodeAttribute)
		.toInt();
	const auto body = ErrorFromBody(reply->readAll());
	if (!body.isEmpty()) {
		return status
			? u"HTTP %1: %2"_q.arg(status).arg(body)
			: body;
	} else if (status) {
		return u"HTTP %1"_q.arg(status);
	} else if (reply->error() != QNetworkReply::NoError) {
		return reply->errorString();
	}
	return u"Unknown error"_q;
}

} // namespace

Stream::Stream(QNetworkReply *reply, StreamHandlers handlers)
: _reply(reply)
, _handlers(std::move(handlers)) {
	setup();
}

Stream::~Stream() {
	cancel();
}

void Stream::setup() {
	if (!_reply) {
		reportFail(u"Request not created"_q);
		return;
	}
	const auto raw = _reply.data();
	QObject::connect(raw, &QNetworkReply::readyRead, raw, crl::guard(this, [=] {
		handleData();
	}));
	QObject::connect(raw, &QNetworkReply::finished, raw, crl::guard(this, [=] {
		handleFinished();
	}));
}

void Stream::cancel() {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	_finished = true;
	_handlers = StreamHandlers();
	if (const auto raw = _reply.data()) {
		_reply = nullptr;
		raw->abort();
		raw->deleteLater();
	}
}

bool Stream::finished() const {
	return _finished;
}

const QString &Stream::accumulated() const {
	return _accumulated;
}

void Stream::handleData() {
	if (_cancelled || !_reply) {
		return;
	}
	_buffer.append(_reply->readAll());
	while (true) {
		const auto index = _buffer.indexOf('\n');
		if (index < 0) {
			break;
		}
		auto line = _buffer.left(index);
		_buffer.remove(0, index + 1);
		if (line.endsWith('\r')) {
			line.chop(1);
		}
		feed(line);
		if (_cancelled) {
			return;
		}
	}
}

void Stream::feed(const QByteArray &line) {
	if (line.isEmpty()) {
		return;
	} else if (!line.startsWith(kDataPrefix)) {
		// llama.cpp also sends "error: {...}" lines on mid-stream failures.
		if (line.startsWith("error:")) {
			reportFail(ErrorFromBody(line.mid(6).trimmed()));
		}
		return;
	}
	const auto payload = line.mid(int(qstrlen(kDataPrefix))).trimmed();
	if (payload == kDoneMarker) {
		_sawDone = true;
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(payload, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return;
	}
	const auto root = document.object();
	if (root.contains(u"error"_q)) {
		reportFail(ErrorFromBody(payload));
		return;
	}
	const auto choices = root.value(u"choices"_q).toArray();
	if (choices.isEmpty()) {
		return;
	}
	const auto choice = choices.at(0).toObject();
	const auto delta = choice.value(u"delta"_q).toObject();
	auto text = delta.value(u"content"_q).toString();
	if (text.isEmpty()) {
		// Non-streaming servers answer with the whole message at once.
		const auto message = choice.value(u"message"_q).toObject();
		text = message.value(u"content"_q).toString();
	}
	if (text.isEmpty()) {
		return;
	}
	_accumulated += text;
	if (const auto onDelta = _handlers.delta) {
		onDelta(text);
	}
}

void Stream::handleFinished() {
	if (_cancelled || _finished) {
		return;
	}
	const auto raw = _reply.data();
	if (!raw) {
		reportFail(u"Request destroyed"_q);
		return;
	}
	handleData();
	if (_cancelled || _finished) {
		return;
	}
	const auto status = raw
		->attribute(QNetworkRequest::HttpStatusCodeAttribute)
		.toInt();
	const auto failed = (raw->error() != QNetworkReply::NoError)
		|| (status && (status < 200 || status >= 300));
	if (failed && _accumulated.isEmpty()) {
		reportFail(DescribeReply(raw));
		return;
	}
	_finished = true;
	const auto onDone = _handlers.done;
	_handlers = StreamHandlers();
	if (onDone) {
		onDone();
	}
}

void Stream::reportFail(const QString &error) {
	if (_finished) {
		return;
	}
	_finished = true;
	const auto onFail = _handlers.fail;
	_handlers = StreamHandlers();
	if (onFail) {
		onFail(error.isEmpty() ? u"Unknown error"_q : error);
	}
}

Client::Client()
: _manager(std::make_unique<QNetworkAccessManager>()) {
}

Client::~Client() = default;

QString Client::endpoint(const QString &path) const {
	return NormalizedBase(Current().server) + path;
}

void Client::requestServerInfo(
		Fn<void(ServerInfo)> done,
		Fn<void(QString)> fail) {
	auto request = QNetworkRequest(QUrl(endpoint(u"/v1/models"_q)));
	const auto key = Current().apiKey;
	if (!key.isEmpty()) {
		request.setRawHeader("Authorization", ("Bearer " + key).toUtf8());
	}
	const auto reply = _manager->get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto guard = gsl::finally([=] { reply->deleteLater(); });
		if (reply->error() != QNetworkReply::NoError) {
			if (fail) {
				fail(DescribeReply(reply));
			}
			return;
		}
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(
			reply->readAll(),
			&error);
		if (error.error != QJsonParseError::NoError
			|| !document.isObject()) {
			if (fail) {
				fail(u"Bad /v1/models answer"_q);
			}
			return;
		}
		auto info = ServerInfo();
		const auto list = document.object().value(u"data"_q).toArray();
		for (const auto &value : list) {
			const auto id = value.toObject().value(u"id"_q).toString();
			if (!id.isEmpty()) {
				info.models.push_back(id);
			}
		}
		if (info.models.empty()) {
			if (fail) {
				fail(u"The server reports no models"_q);
			}
			return;
		}
		if (done) {
			done(std::move(info));
		}
	});
}

std::shared_ptr<Stream> Client::start(
		GenerationRequest request,
		StreamHandlers handlers) {
	auto messages = QJsonArray();
	for (const auto &message : request.messages) {
		auto object = QJsonObject();
		object.insert(u"role"_q, message.role);
		object.insert(u"content"_q, message.content);
		messages.append(object);
	}
	auto body = QJsonObject();
	if (!request.model.isEmpty()) {
		body.insert(u"model"_q, request.model);
	}
	body.insert(u"messages"_q, messages);
	body.insert(u"stream"_q, true);
	body.insert(u"temperature"_q, request.temperature);
	body.insert(u"top_p"_q, request.topP);
	if (request.maxTokens > 0) {
		body.insert(u"max_tokens"_q, request.maxTokens);
	}

	auto networkRequest = QNetworkRequest(
		QUrl(endpoint(u"/v1/chat/completions"_q)));
	networkRequest.setHeader(
		QNetworkRequest::ContentTypeHeader,
		"application/json");
	networkRequest.setRawHeader("Accept", "text/event-stream");
	const auto key = Current().apiKey;
	if (!key.isEmpty()) {
		networkRequest.setRawHeader(
			"Authorization",
			("Bearer " + key).toUtf8());
	}
	// A local model may think for a long while before the first token.
	networkRequest.setTransferTimeout(0);

	const auto reply = _manager->post(
		networkRequest,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	return std::make_shared<Stream>(reply, std::move(handlers));
}

} // namespace LocalAi
