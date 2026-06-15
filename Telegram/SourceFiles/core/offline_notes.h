/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

namespace Core::OfflineNotes {

// This branch turns Telegram Desktop into a single, nameless, fully-offline
// chat - a personal note log. Everything keys off this one flag so the
// behaviour can be reasoned about (and, later, gated) in one place.
[[nodiscard]] bool Enabled();

// The synthesized "self" user that owns the single chat. Stable across runs
// so loaded notes always attach to the same peer.
[[nodiscard]] UserId SelfUserId();

// Folder holding messages.md + media/ (see storage/offline/).
[[nodiscard]] QString FolderPath();

// The embedded dark palette applied as the default theme in offline mode.
[[nodiscard]] QByteArray PaletteData();

// The embedded "Locker" chat avatar (a white star on black) as PNG bytes.
[[nodiscard]] QByteArray AvatarData();

} // namespace Core::OfflineNotes
