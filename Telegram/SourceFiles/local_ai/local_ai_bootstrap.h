/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Account;
} // namespace Main

namespace LocalAi {

// In local mode there is nothing to sign in to, so an unauthorized account
// gets a synthetic self user and a session right away, instead of the intro.
// Returns true if this call created the session.
bool CreateLocalSession(not_null<Main::Account*> account);

} // namespace LocalAi
