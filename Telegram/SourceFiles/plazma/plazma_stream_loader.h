// Plazma port — HTTP streaming loader for tdesktop's Media::Streaming.
//
// This is the bridge that lets Telegram Desktop's OWN video pipeline
// (Media::Streaming::Reader/Player + FFmpeg hwaccel + the media viewer) play a
// self-hosted PlazmaServer URL. It implements the same part-based Loader
// contract as LoaderLocal / LoaderMtproto, but each 128 KiB part is fetched
// with a ranged HTTP GET — the exact mechanic Plazma already ships in
// src/api.cpp (startDownload, Range: bytes=N-) and src/storage/part_file.cpp.
//
// Rationale + the "combine the best" decision: dev/plazma/DECISION_player_synthesis.md.
//
// Header sketch only (no .cpp yet, hence not in CMakeLists) — the structure is
// a faithful mirror of media/streaming/media_streaming_loader_mtproto.h with
// MTP::Sender replaced by QNetworkAccessManager.
#pragma once

#include "media/streaming/media_streaming_loader.h"
#include "base/weak_ptr.h"

#include <QtCore/QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace Plazma {

// Streams one remote URL for Media::Streaming. Construct with the absolute
// media URL (VideoItem::url) and, if known, the total size; if size is 0 the
// loader learns it from the first response's Content-Range and reports it.
class LoaderHttp final
	: public Media::Streaming::Loader
	, public base::has_weak_ptr {
public:
	LoaderHttp(
		not_null<QNetworkAccessManager*> nam,
		QUrl url,
		int64 size = 0,
		QByteArray authToken = {});
	~LoaderHttp();

	// Cache key derived from a hash of the URL (stable across sessions so
	// tdesktop's disk cache can reuse already-fetched parts).
	[[nodiscard]] Storage::Cache::Key baseCacheKey() const override;
	[[nodiscard]] int64 size() const override;

	// load(offset): issue GET url with `Range: bytes=offset-(offset+kPartSize-1)`.
	// On finished, feed {offset, bytes} into _parts (main thread). 206 = ranged
	// ok; 200 = server ignored Range (fall back to reading the needed slice).
	void load(int64 offset) override;
	void cancel(int64 offset) override;
	void resetPriorities() override;
	void setPriority(int priority) override;
	void stop() override;

	void tryRemoveFromQueue() override;

	// Parts are delivered on the main thread, same contract as the other loaders.
	[[nodiscard]] rpl::producer<Media::Streaming::LoadedPart> parts() const override;
	[[nodiscard]] auto speedEstimate() const
		-> rpl::producer<Media::Streaming::SpeedEstimate> override;

	void attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) override;
	void clearAttachedDownloader() override;

private:
	void sendRange(int64 offset);
	void feedPart(int64 offset, const QByteArray &bytes);
	void fail();

	const not_null<QNetworkAccessManager*> _nam;
	const QUrl _url;
	const QByteArray _authToken;
	int64 _size = 0; // 0 until learned from Content-Range.
	int _priority = 0;

	Media::Streaming::PriorityQueue _requested;
	base::flat_map<int64, QNetworkReply*> _inFlight; // offset -> reply
	rpl::event_stream<Media::Streaming::LoadedPart> _parts;
	rpl::event_stream<Media::Streaming::SpeedEstimate> _speedEstimate;

	Storage::StreamedFileDownloader *_downloader = nullptr;
};

} // namespace Plazma
