// Plazma port — session bridge implementation.
#include "plazma/plazma_session.h"

#include "main/main_session.h"
#include "data/data_user.h"

namespace Plazma {

UserLogin UserLoginFromSession(not_null<Main::Session*> session) {
	const auto user = session->user();
	auto login = UserLogin();
	// Main::Session::userId() is a strong UserId wrapping a BareId (int64),
	// which is exactly PlazmaServer's user_id.
	login.userId = qint64(session->userId().bare);
	login.username = user->username();
	login.firstName = user->firstName;
	login.lastName = user->lastName;
	login.phoneNumber = user->phone();
	login.isPremium = user->isPremium();
	return login;
}

Backend::Backend(not_null<Main::Session*> session)
: _session(session)
, _api(std::make_unique<Api>()) {
}

Backend::~Backend() = default;

void Backend::start() {
	_api->loginUser(UserLoginFromSession(_session));
}

} // namespace Plazma
