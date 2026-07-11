#include "plazma/plazma_api.h"

#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QHttpMultiPart>

namespace Plazma {
namespace {

const auto kPaths = std::unordered_map<Endpoint, QString>{
	{ Endpoint::AuthLogin, QStringLiteral("/v1/auth/login") },
	{ Endpoint::Videos, QStringLiteral("/v1/videos") },
	{ Endpoint::VideosUpload, QStringLiteral("/v1/videos/upload") },
	{ Endpoint::Playlists, QStringLiteral("/v1/playlists") },
};

} // namespace

QString ToEndpointString(Endpoint endpoint) {
	return kPaths.at(endpoint);
}

QByteArray ToMethodString(HttpMethod method) {
	switch (method) {
	case HttpMethod::Get: return "GET";
	case HttpMethod::Post: return "POST";
	case HttpMethod::Head: return "HEAD";
	case HttpMethod::Put: return "PUT";
	case HttpMethod::Delete: return "DELETE";
	case HttpMethod::Patch: return "PATCH";
	case HttpMethod::Options: return "OPTIONS";
	}
	Q_UNREACHABLE();
	return "GET";
}

VideoItem VideoItem::fromJson(const QJsonObject &o) {
	auto item = VideoItem();
	item.id = o.value(u"id"_q).toString();
	item.title = o.value(u"title"_q).toString();
	item.url = o.value(u"url"_q).toString();
	item.size = qint64(o.value(u"size"_q).toDouble());
	item.mime = o.value(u"mime"_q).toString();
	item.author = o.value(u"author"_q).toString();
	item.createdAt = o.value(u"created_at"_q).toString();
	item.thumbnail = o.value(u"thumbnail"_q).toString();
	item.storyboard = o.value(u"storyboard"_q).toString();
	item.description = o.value(u"description"_q).toString();
	return item;
}

RequestBuilder::RequestBuilder(
	QNetworkAccessManager *nam,
	QNetworkRequest req,
	HttpMethod method,
	QByteArray body)
: _nam(nam)
, _req(std::make_unique<QNetworkRequest>(std::move(req)))
, _method(method)
, _body(std::move(body)) {
}

RequestBuilder::RequestBuilder(
	QNetworkAccessManager *nam,
	QNetworkRequest req,
	QHttpMultiPart *multiPart)
: _nam(nam)
, _req(std::make_unique<QNetworkRequest>(std::move(req)))
, _method(HttpMethod::Post)
, _multiPart(multiPart) {
}

RequestBuilder &RequestBuilder::done(Fn<QJsonObject> cb) {
	_done = std::move(cb);
	return *this;
}

RequestBuilder &RequestBuilder::fail(Fn<int, QString> cb) {
	_fail = std::move(cb);
	return *this;
}

void RequestBuilder::send() {
	QNetworkReply *reply = nullptr;
	if (_multiPart) {
		reply = _nam->post(*_req, _multiPart);
		_multiPart->setParent(reply);
	} else {
		reply = _nam->sendCustomRequest(*_req, ToMethodString(_method), _body);
	}

	QObject::connect(reply, &QNetworkReply::finished, reply, [=, done = std::move(_done), fail = std::move(_fail)] {
		if (reply->error() != QNetworkReply::NoError) {
			if (fail) {
				const auto code = reply
					->attribute(QNetworkRequest::HttpStatusCodeAttribute)
					.toInt();
				fail(code, reply->errorString());
			}
		} else if (done) {
			const auto doc = QJsonDocument::fromJson(reply->readAll());
			done(doc.object());
		}
		reply->deleteLater();
	});
}

Api::Api(QObject *parent)
: QObject(parent)
, _nam(new QNetworkAccessManager(this)) {
}

Api::~Api() = default;

void Api::setBaseUrl(QString baseUrl) {
	_baseUrl = std::move(baseUrl);
}

void Api::applyAuth(QNetworkRequest &req) const {
	if (!_authToken.isEmpty()) {
		req.setRawHeader("Authorization", "Bearer " + _authToken);
	}
}

RequestBuilder Api::request(
	Endpoint endpoint,
	const QJsonObject &body,
	HttpMethod method) {
	return request(endpoint, QUrlQuery{}, body, method);
}

RequestBuilder Api::request(
	Endpoint endpoint,
	const QUrlQuery &params,
	const QJsonObject &body,
	HttpMethod method) {
	auto url = QUrl(_baseUrl + ToEndpointString(endpoint));
	if (!params.isEmpty()) {
		url.setQuery(params);
	}
	auto req = QNetworkRequest(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setRawHeader("Connection", "close");
	applyAuth(req);
	return RequestBuilder(
		_nam,
		std::move(req),
		method,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
}

RequestBuilder Api::requestPath(
	const QString &path,
	const QJsonObject &body,
	HttpMethod method) {
	auto req = QNetworkRequest(QUrl(_baseUrl + path));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setRawHeader("Connection", "close");
	applyAuth(req);
	return RequestBuilder(
		_nam,
		std::move(req),
		method,
		body.isEmpty()
			? QByteArray()
			: QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void Api::loginUser(const UserLogin &user) {
	const auto body = QJsonObject{
		{ u"user_id"_q, user.userId },
		{ u"username"_q, user.username },
		{ u"first_name"_q, user.firstName },
		{ u"last_name"_q, user.lastName },
		{ u"phone_number"_q, user.phoneNumber },
		{ u"is_premium"_q, user.isPremium },
	};
	request(Endpoint::AuthLogin, body, HttpMethod::Post)
		.done([=](const QJsonObject &json) {
			const auto token = json.value(u"token"_q).toString();
			if (token.isEmpty()) {
				Q_EMIT loginError(0, u"login response missing token"_q);
				return;
			}
			setAuthToken(token.toUtf8());

			auto result = UserLogin();
			const auto u = json.value(u"user"_q).toObject();
			result.userId = qint64(u.value(u"user_id"_q).toDouble());
			result.username = u.value(u"username"_q).toString();
			result.firstName = u.value(u"first_name"_q).toString();
			result.lastName = u.value(u"last_name"_q).toString();
			result.phoneNumber = u.value(u"phone_number"_q).toString();
			result.isPremium = u.value(u"is_premium"_q).toBool();
			Q_EMIT loginSuccess(result);
		})
		.fail([=](int statusCode, const QString &error) {
			Q_EMIT loginError(statusCode, error);
		})
		.send();
}

void Api::uploadVideo(
	const QString &filename,
	const QString &mime,
	const QByteArray &filedata,
	const QByteArray &thumbnail,
	const QString &thumbnailMime) {
	auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

	auto filePart = QHttpPart();
	filePart.setHeader(QNetworkRequest::ContentTypeHeader, mime);
	filePart.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(filename));
	filePart.setBody(filedata);
	multiPart->append(filePart);

	// Optional optimistic thumbnail — server keys off name="thumbnail" and
	// skips its own ffmpeg extraction when present.
	if (!thumbnail.isEmpty()) {
		auto thumbPart = QHttpPart();
		thumbPart.setHeader(QNetworkRequest::ContentTypeHeader, thumbnailMime);
		thumbPart.setHeader(
			QNetworkRequest::ContentDispositionHeader,
			QStringLiteral("form-data; name=\"thumbnail\"; filename=\"thumb.jpg\""));
		thumbPart.setBody(thumbnail);
		multiPart->append(thumbPart);
	}

	auto url = QUrl(_baseUrl + ToEndpointString(Endpoint::VideosUpload));
	auto req = QNetworkRequest(url);
	applyAuth(req);
	RequestBuilder(_nam, std::move(req), multiPart)
		.done([=](const QJsonObject &) {
			Q_EMIT uploadFinished(filename);
		})
		.fail([=](int code, const QString &error) {
			Q_EMIT uploadFailed(code, error);
		})
		.send();
}

void Api::fetchVideos(
	const QString &query,
	Fn<QList<VideoItem>> onSuccess,
	Fn<int, QString> onError) {
	auto params = QUrlQuery();
	const auto trimmed = query.trimmed();
	if (!trimmed.isEmpty()) {
		params.addQueryItem(u"q"_q, trimmed);
	}
	request(Endpoint::Videos, params, {}, HttpMethod::Get)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (!done) {
				return;
			}
			auto out = QList<VideoItem>();
			const auto arr = json.value(u"videos"_q).toArray();
			out.reserve(arr.size());
			for (const auto &v : arr) {
				out.push_back(VideoItem::fromJson(v.toObject()));
			}
			done(out);
		})
		.fail(std::move(onError))
		.send();
}

PlaylistItem PlaylistItem::fromJson(const QJsonObject &o) {
	// Wire shape: PlazmaServer/docs/playlists.md §2.3. Every string field is a
	// defined string (never null) per the contract; duration_ms may be null.
	auto item = PlaylistItem();
	item.videoId = o.value(u"video_id"_q).toString();
	item.title = o.value(u"title"_q).toString();
	item.url = o.value(u"url"_q).toString();
	item.thumbnail = o.value(u"thumbnail"_q).toString();
	item.storyboard = o.value(u"storyboard"_q).toString();
	item.author = o.value(u"author"_q).toString();
	item.mime = o.value(u"mime"_q).toString();
	item.description = o.value(u"description"_q).toString();
	item.size = qint64(o.value(u"size"_q).toDouble());
	const auto duration = o.value(u"duration_ms"_q);
	item.durationMs = duration.isNull() ? -1 : qint64(duration.toDouble());
	item.addedAt = o.value(u"added_at"_q).toString();
	return item;
}

QJsonObject PlaylistItem::toSnapshot() const {
	// Client-supplied snapshot for POST .../items (§3.7). added_at is omitted
	// on purpose — the server stamps it authoritatively.
	auto snapshot = QJsonObject{
		{ u"video_id"_q, videoId },
		{ u"title"_q, title },
		{ u"url"_q, url },
		{ u"thumbnail"_q, thumbnail },
		{ u"storyboard"_q, storyboard },
		{ u"mime"_q, mime },
		{ u"size"_q, double(size) },
		{ u"author"_q, author },
		{ u"description"_q, description },
	};
	if (durationMs >= 0) {
		snapshot.insert(u"duration_ms"_q, double(durationMs));
	}
	return snapshot;
}

Playlist Playlist::fromJson(const QJsonObject &o) {
	// Summary shape: §2.1. Detail responses (§2.2) additionally carry items[].
	auto playlist = Playlist();
	playlist.id = o.value(u"id"_q).toString();
	playlist.name = o.value(u"name"_q).toString();
	playlist.createdAt = o.value(u"created_at"_q).toString();
	playlist.updatedAt = o.value(u"updated_at"_q).toString();
	const auto covers = o.value(u"cover_thumbnails"_q).toArray();
	playlist.coverThumbnails.reserve(covers.size());
	for (const auto &c : covers) {
		playlist.coverThumbnails.push_back(c.toString());
	}
	const auto items = o.value(u"items"_q).toArray();
	playlist.items.reserve(items.size());
	for (const auto &v : items) {
		playlist.items.push_back(PlaylistItem::fromJson(v.toObject()));
	}
	// `video_count` is the authoritative server count; fall back to item_count,
	// then to however many items we actually parsed.
	playlist.itemCount = o.contains(u"video_count"_q)
		? o.value(u"video_count"_q).toInt()
		: (o.contains(u"item_count"_q)
			? o.value(u"item_count"_q).toInt()
			: int(playlist.items.size()));
	return playlist;
}

void Api::listPlaylists(
	Fn<QList<Playlist>> onSuccess,
	Fn<int, QString> onError) {
	request(Endpoint::Playlists, {}, HttpMethod::Get)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (!done) {
				return;
			}
			auto out = QList<Playlist>();
			const auto arr = json.value(u"playlists"_q).toArray();
			out.reserve(arr.size());
			for (const auto &v : arr) {
				out.push_back(Playlist::fromJson(v.toObject()));
			}
			done(out);
		})
		.fail(std::move(onError))
		.send();
}

void Api::createPlaylist(
	const QString &id,
	const QString &name,
	Fn<Playlist> onSuccess,
	Fn<int, QString> onError) {
	const auto body = QJsonObject{ { u"id"_q, id }, { u"name"_q, name } };
	request(Endpoint::Playlists, body, HttpMethod::Post)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (done) {
				done(Playlist::fromJson(json.value(u"playlist"_q).toObject()));
			}
		})
		.fail(std::move(onError))
		.send();
}

void Api::renamePlaylist(
	const QString &id,
	const QString &newName,
	Fn<Playlist> onSuccess,
	Fn<int, QString> onError) {
	const auto body = QJsonObject{ { u"name"_q, newName } };
	requestPath(u"/v1/playlists/"_q + id, body, HttpMethod::Patch)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (done) {
				done(Playlist::fromJson(json.value(u"playlist"_q).toObject()));
			}
		})
		.fail(std::move(onError))
		.send();
}

void Api::deletePlaylist(
	const QString &id,
	Fn<> onSuccess,
	Fn<int, QString> onError) {
	requestPath(u"/v1/playlists/"_q + id, {}, HttpMethod::Delete)
		.done([done = std::move(onSuccess)](const QJsonObject &) {
			if (done) {
				done();
			}
		})
		.fail(std::move(onError))
		.send();
}

void Api::addPlaylistItem(
	const QString &playlistId,
	const PlaylistItem &item,
	Fn<PlaylistItem, Playlist> onSuccess,
	Fn<int, QString> onError) {
	const auto path = u"/v1/playlists/"_q + playlistId + u"/items"_q;
	requestPath(path, item.toSnapshot(), HttpMethod::Post)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			const auto item = json.value(u"item"_q).toObject();
			if (item.isEmpty()) {
				return;
			}
			if (done) {
				done(
					PlaylistItem::fromJson(item),
					Playlist::fromJson(json.value(u"playlist"_q).toObject()));
			}
		})
		.fail(std::move(onError))
		.send();
}

void Api::removePlaylistItem(
	const QString &playlistId,
	const QString &videoId,
	Fn<> onSuccess,
	Fn<int, QString> onError) {
	const auto path = u"/v1/playlists/"_q
		+ playlistId
		+ u"/items/"_q
		+ videoId;
	requestPath(path, {}, HttpMethod::Delete)
		.done([done = std::move(onSuccess)](const QJsonObject &) {
			if (done) {
				done();
			}
		})
		.fail(std::move(onError))
		.send();
}

void Api::getPlaylist(
	const QString &id,
	Fn<Playlist> onSuccess,
	Fn<int, QString> onError) {
	requestPath(u"/v1/playlists/"_q + id, {}, HttpMethod::Get)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (!done) {
				return;
			}
			// Detail response nests the summary under "playlist" and the first
			// page under "items"; merge them so Playlist::fromJson sees both.
			auto merged = json.value(u"playlist"_q).toObject();
			merged.insert(u"items"_q, json.value(u"items"_q).toArray());
			done(Playlist::fromJson(merged));
		})
		.fail(std::move(onError))
		.send();
}

void Api::playlistsByVideo(
	const QString &videoId,
	Fn<QStringList> onSuccess,
	Fn<int, QString> onError) {
	const auto path = u"/v1/users/me/playlists/by_video/"_q + videoId;
	requestPath(path, {}, HttpMethod::Get)
		.done([done = std::move(onSuccess)](const QJsonObject &json) {
			if (!done) {
				return;
			}
			auto ids = QStringList();
			const auto arr = json.value(u"playlist_ids"_q).toArray();
			ids.reserve(arr.size());
			for (const auto &v : arr) {
				ids.push_back(v.toString());
			}
			done(ids);
		})
		.fail(std::move(onError))
		.send();
}

} // namespace Plazma
