/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace LocalAi {

// Server address, credentials, sampling parameters and the system prompt.
void SettingsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

// Asks the server for /v1/models and reports the outcome as a toast.
void RefreshModels(not_null<Window::SessionController*> controller);

} // namespace LocalAi
