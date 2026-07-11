// Plazma port — the one file that couples the backend client to tdesktop.
//
// Plazma's stock login runs a *second* Telegram client (TDLib) purely to learn
// the user's identity, then POSTs it to PlazmaServer's /v1/auth/login. Inside
// Telegram Desktop that identity is already sitting in the logged-in MTP
// session, so there is no TDLib and no second client — this bridge reads
// Main::Session::user() and drives Plazma::Api::loginUser().
//
// Keep tdesktop <-> PlazmaServer coupling confined to this translation unit:
// plazma_api.{h,cpp} stays Qt-only and standalone-compilable; everything that
// needs Main::Session goes here.
#pragma once

#include "plazma/plazma_api.h"

#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Plazma {

// Snapshots the logged-in Telegram user into the exact shape PlazmaServer's
// TelegramLoginDTO expects (user_id/username/first_name/last_name/phone_number/
// is_premium). Safe to call any time after intro/ has completed.
[[nodiscard]] UserLogin UserLoginFromSession(not_null<Main::Session*> session);

// Owns the PlazmaServer REST client for one Telegram session and performs the
// identity handshake. Construct once the session is authorized; call start()
// to log in to the backend. After loginSuccess fires, api() is authed and can
// serve the feed / uploads / playlists.
class Backend final {
public:
	explicit Backend(not_null<Main::Session*> session);
	~Backend();

	// POSTs the current session identity to /v1/auth/login. Idempotent on the
	// server (it's an upsert); calling twice just refreshes the bearer token.
	void start();

	[[nodiscard]] Api &api() {
		return *_api;
	}
	[[nodiscard]] bool loggedIn() const {
		return _api->hasAuthToken();
	}

private:
	const not_null<Main::Session*> _session;
	const std::unique_ptr<Api> _api;
};

} // namespace Plazma
