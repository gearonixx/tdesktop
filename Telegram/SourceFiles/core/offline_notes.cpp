/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/offline_notes.h"

#include "settings.h"

namespace Core::OfflineNotes {

bool Enabled() {
	return true;
}

UserId SelfUserId() {
	// Arbitrary fixed id for the local self user; never leaves this machine.
	return UserId(708111708111);
}

QString FolderPath() {
	return cWorkingDir() + u"notes"_q;
}

} // namespace Core::OfflineNotes
