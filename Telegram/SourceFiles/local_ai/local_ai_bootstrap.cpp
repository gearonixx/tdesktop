/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_bootstrap.h"

#include "local_ai/local_ai_config.h"
#include "main/main_account.h"
#include "main/main_session_settings.h"

namespace LocalAi {
namespace {

[[nodiscard]] MTPUser MakeSelfUser() {
	using Flag = MTPDuser::Flag;
	auto name = Current().selfName.trimmed();
	if (name.isEmpty()) {
		name = u"You"_q;
	}
	const auto flags = Flag::f_self | Flag::f_first_name;
	return MTP_user(
		MTP_flags(flags),
		MTP_long(kSelfUserId),
		MTPlong(), // access_hash
		MTP_string(name),
		MTPstring(), // last_name
		MTPstring(), // username
		MTPstring(), // phone
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTPint(), // bot_info_version
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

} // namespace

bool CreateLocalSession(not_null<Main::Account*> account) {
	if (!Enabled() || account->sessionExists()) {
		return false;
	}
	auto settings = std::make_unique<Main::SessionSettings>();
	settings->setDialogsFiltersEnabled(false);
	account->createSession(MakeSelfUser(), std::move(settings));
	return account->sessionExists();
}

} // namespace LocalAi
