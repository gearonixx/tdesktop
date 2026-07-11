// Plazma port — self-hosted media backend client.
//
// This is a faithful port of Plazma's `src/api.{h,cpp}` down to the same
// REST contract and the same fluent RequestBuilder ergonomics, but stripped
// of the QML/Qt Quick coupling so it can live inside Telegram Desktop and be
// driven by tdesktop's own Ui widgets.
//
// It intentionally depends only on Qt Core + Qt Network (both already linked
// by tdesktop), so this translation unit compiles standalone and is the safe
// first brick of the port. See dev/plazma/PLAZMA_PORT_REPORT.md for how it
// slots into the window/section machinery.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QUrlQuery>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QHttpMultiPart;

namespace Plazma {

template <typename... Args>
using Fn = std::function<void(Args...)>;

enum class HttpMethod { Get, Post, Head, Put, Delete, Patch, Options };
enum class Endpoint { AuthLogin, Videos, VideosUpload, Playlists };

// Identity handed to /v1/auth/login. In stock Plazma these fields come out of
// TDLib's authorizationStateReady. Inside tdesktop the same values are already
// available from the logged-in MTP session (Main::Session::user()), so the
// port fills this from there instead of running a second Telegram client.
struct UserLogin final {
	qint64 userId = 0;
	QString username;
	QString firstName;
	QString lastName;
	QString phoneNumber;
	bool isPremium = false;
};

// One feed row — mirrors VideoFeedModel::VideoItem and the wire shape
// documented in api.cpp's feed contract.
struct VideoItem final {
	QString id;
	QString title;
	QString url;
	qint64 size = 0;
	QString mime;
	QString author;
	QString createdAt;
	QString thumbnail;
	QString storyboard;
	QString description;

	[[nodiscard]] static VideoItem fromJson(const QJsonObject &o);
};

// A single entry inside a playlist. The server stores a *snapshot* of the video
// at add-time (see PlazmaServer/docs/playlists.md §3.7) so a playlist row still
// renders if the source video is later renamed/deleted.
struct PlaylistItem final {
	QString videoId;
	QString title;
	QString url;
	QString thumbnail;
	QString storyboard;
	QString author;
	QString mime;
	QString description;
	qint64 size = 0;
	qint64 durationMs = -1; // -1 == unprobed (server sends null).
	QString addedAt;        // server-stamped ISO-8601 UTC; authoritative.

	[[nodiscard]] static PlaylistItem fromJson(const QJsonObject &o);
	[[nodiscard]] QJsonObject toSnapshot() const; // body for addPlaylistItem
};

// A playlist. Rendered as a Telegram-style *folder tab* over the feed (see
// dev/plazma/DECISION_playlists_folders.md — we deliberately do NOT route this
// through Data::ChatFilters, which is peer-centric). Fields mirror the frozen
// summary shape in PlazmaServer/docs/playlists.md §2.1.
struct Playlist final {
	QString id;
	QString name;
	QString createdAt;
	QString updatedAt;
	int itemCount = 0;              // server `video_count` (authoritative).
	QStringList coverThumbnails;    // up to 4, newest-first — the grid mosaic.
	QList<PlaylistItem> items;      // only the locally-loaded subset.

	[[nodiscard]] static Playlist fromJson(const QJsonObject &o);
};

[[nodiscard]] QString ToEndpointString(Endpoint endpoint);
[[nodiscard]] QByteArray ToMethodString(HttpMethod method);

// Fluent one-shot request. Chain .done()/.fail() then .send(); dropping the
// builder without .send() never fires the request (same footgun the original
// documents), so callers must terminate the chain.
class RequestBuilder final {
public:
	RequestBuilder(
		QNetworkAccessManager *nam,
		QNetworkRequest req,
		HttpMethod method,
		QByteArray body);
	RequestBuilder(
		QNetworkAccessManager *nam,
		QNetworkRequest req,
		QHttpMultiPart *multiPart);

	RequestBuilder &done(Fn<QJsonObject> cb);
	RequestBuilder &fail(Fn<int, QString> cb);
	void send();

private:
	QNetworkAccessManager *_nam = nullptr;
	std::unique_ptr<QNetworkRequest> _req;
	HttpMethod _method = HttpMethod::Get;
	QByteArray _body;
	QHttpMultiPart *_multiPart = nullptr;
	Fn<QJsonObject> _done;
	Fn<int, QString> _fail;
};

class Api final : public QObject {
	Q_OBJECT

public:
	explicit Api(QObject *parent = nullptr);
	~Api();

	// Point the client at a PlazmaServer instance. Defaults to the same
	// http://localhost:8080 the original hard-codes.
	void setBaseUrl(QString baseUrl);
	[[nodiscard]] QString baseUrl() const { return _baseUrl; }

	[[nodiscard]] RequestBuilder request(
		Endpoint endpoint,
		const QJsonObject &body = {},
		HttpMethod method = HttpMethod::Get);
	[[nodiscard]] RequestBuilder request(
		Endpoint endpoint,
		const QUrlQuery &params,
		const QJsonObject &body = {},
		HttpMethod method = HttpMethod::Get);

	void loginUser(const UserLogin &user);

	// GET /v1/videos[?q=]. Empty query -> full chronological feed.
	void fetchVideos(
		const QString &query,
		Fn<QList<VideoItem>> onSuccess,
		Fn<int, QString> onError = {});

	// POST /v1/videos/upload (multipart/form-data). `filedata` is the raw
	// video bytes; `thumbnail` is an optional pre-rendered JPEG the server
	// stores optimistically (skips its own ffmpeg extraction). Mirrors the
	// original Api::uploadFile down to the field names.
	void uploadVideo(
		const QString &filename,
		const QString &mime,
		const QByteArray &filedata,
		const QByteArray &thumbnail = {},
		const QString &thumbnailMime = QStringLiteral("image/jpeg"));

	// ─── Playlists ──────────────────────────────────────────────────────────
	// All playlist calls require auth and are scoped to the caller. Wire format
	// is authoritative in PlazmaServer/docs/playlists.md. The client supplies
	// its own id on create (UUID v7) so the UI can build the folder optimistically
	// and reconcile on the server reply — see plazma_playlists.md.
	void listPlaylists(
		Fn<QList<Playlist>> onSuccess,
		Fn<int, QString> onError = {});
	void createPlaylist(
		const QString &id,
		const QString &name,
		Fn<Playlist> onSuccess = {},
		Fn<int, QString> onError = {});
	void renamePlaylist(
		const QString &id,
		const QString &newName,
		Fn<Playlist> onSuccess = {},
		Fn<int, QString> onError = {});
	void deletePlaylist(
		const QString &id,
		Fn<> onSuccess = {},
		Fn<int, QString> onError = {});
	void addPlaylistItem(
		const QString &playlistId,
		const PlaylistItem &item,
		Fn<PlaylistItem, Playlist> onSuccess = {},
		Fn<int, QString> onError = {});
	void removePlaylistItem(
		const QString &playlistId,
		const QString &videoId,
		Fn<> onSuccess = {},
		Fn<int, QString> onError = {});
	// GET /v1/playlists/{id} — metadata + first page of items in one trip.
	// Used by openPlaylist() to lazily hydrate a folder's item list.
	void getPlaylist(
		const QString &id,
		Fn<Playlist> onSuccess,
		Fn<int, QString> onError = {});
	// GET /v1/users/me/playlists/by_video/{videoId} — the ids of my playlists
	// that contain this video (drives the saved-state badge on the feed).
	void playlistsByVideo(
		const QString &videoId,
		Fn<QStringList> onSuccess,
		Fn<int, QString> onError = {});

	void setAuthToken(QByteArray token) { _authToken = std::move(token); }
	[[nodiscard]] bool hasAuthToken() const { return !_authToken.isEmpty(); }

Q_SIGNALS:
	void loginSuccess(Plazma::UserLogin user);
	void loginError(int statusCode, QString error);
	void uploadFinished(QString filename);
	void uploadFailed(int statusCode, QString error);

private:
	void applyAuth(QNetworkRequest &req) const;

	// Build a request against an arbitrary base-relative path. Playlist item
	// endpoints have variable ids (`/v1/playlists/{id}/items/{videoId}`) that
	// the fixed Endpoint enum can't express, so those go through here — the
	// port's equivalent of Plazma's buildPlaylistRequest().
	[[nodiscard]] RequestBuilder requestPath(
		const QString &path,
		const QJsonObject &body,
		HttpMethod method);

	QNetworkAccessManager *_nam = nullptr;
	QString _baseUrl = QStringLiteral("http://localhost:8080");
	QByteArray _authToken;
};

} // namespace Plazma
