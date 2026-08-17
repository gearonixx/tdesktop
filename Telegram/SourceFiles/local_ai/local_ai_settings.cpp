/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "local_ai/local_ai_settings.h"

#include "local_ai/local_ai_chats.h"
#include "local_ai/local_ai_config.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace LocalAi {
namespace {

[[nodiscard]] not_null<Ui::InputField*> AddField(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		const QString &placeholder,
		const QString &value,
		Ui::InputField::Mode mode = Ui::InputField::Mode::SingleLine) {
	Ui::AddSkip(box->verticalLayout());
	Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(title));
	return box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		mode,
		rpl::single(placeholder),
		value));
}

[[nodiscard]] QString NumberText(float64 value) {
	return QString::number(value, 'g', 4);
}

} // namespace

void RefreshModels(not_null<Window::SessionController*> controller) {
	const auto show = controller->uiShow();
	controller->session().localAi().refreshModels([=] {
		show->showToast(u"Model list is up to date."_q);
	}, [=](QString error) {
		show->showToast(u"llama.cpp server: %1"_q.arg(error));
	});
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto &current = Current();

	box->setTitle(rpl::single(u"Local AI"_q));

	const auto content = box->verticalLayout();
	Ui::AddDividerText(content, rpl::single(
		u"Chats in this app are conversations with the models served by a "
		"llama.cpp server. Point it anywhere you like -- a local process, or "
		"a rented GPU box reachable over the network."_q));

	const auto server = AddField(
		box,
		u"Server"_q,
		u"http://127.0.0.1:8080"_q,
		current.server);
	const auto apiKey = AddField(
		box,
		u"API key (optional)"_q,
		u"--api-key value, if the server needs one"_q,
		current.apiKey);
	const auto prompt = AddField(
		box,
		u"System prompt"_q,
		u"How the model should behave"_q,
		current.systemPrompt,
		Ui::InputField::Mode::MultiLine);
	const auto temperature = AddField(
		box,
		u"Temperature"_q,
		u"0.7"_q,
		NumberText(current.temperature));
	const auto topP = AddField(
		box,
		u"Top P"_q,
		u"0.95"_q,
		NumberText(current.topP));
	const auto maxTokens = AddField(
		box,
		u"Answer limit, tokens"_q,
		u"2048"_q,
		QString::number(current.maxTokens));
	const auto contextMessages = AddField(
		box,
		u"Messages kept as context"_q,
		u"24"_q,
		QString::number(current.contextMessages));
	const auto selfName = AddField(
		box,
		u"Your name"_q,
		u"You"_q,
		current.selfName);

	Ui::AddSkip(content);
	const auto enabled = content->add(
		object_ptr<Ui::Checkbox>(
			content,
			u"Local AI mode"_q,
			current.enabled,
			st::defaultBoxCheckbox),
		st::boxRowPadding);
	Ui::AddDividerText(content, rpl::single(
		u"Turning local mode off makes this a normal Telegram client again "
		"after a restart. Turning it on keeps the app fully offline: no "
		"MTProto session is opened at all."_q));

	const auto show = controller->uiShow();
	const auto save = [=] {
		auto &config = Current();
		const auto wasEnabled = config.enabled;
		const auto wasServer = config.server;

		config.server = server->getLastText().trimmed();
		config.apiKey = apiKey->getLastText().trimmed();
		config.systemPrompt = prompt->getLastText();
		config.selfName = selfName->getLastText().trimmed();
		config.enabled = enabled->checked();

		auto ok = false;
		if (const auto value = temperature->getLastText().toDouble(&ok); ok) {
			config.temperature = std::clamp(value, 0., 2.);
		}
		if (const auto value = topP->getLastText().toDouble(&ok); ok) {
			config.topP = std::clamp(value, 0., 1.);
		}
		if (const auto value = maxTokens->getLastText().toInt(&ok); ok) {
			config.maxTokens = std::clamp(value, 0, 1024 * 1024);
		}
		if (const auto value = contextMessages->getLastText().toInt(&ok); ok) {
			config.contextMessages = std::clamp(value, 2, 4096);
		}
		Save();

		if (config.enabled != wasEnabled) {
			show->showToast(u"Restart the app to apply the mode change."_q);
		} else if (config.server != wasServer) {
			RefreshModels(controller);
		}
		box->closeBox();
	};

	box->addButton(rpl::single(u"Save"_q), save);
	box->addLeftButton(rpl::single(u"Refresh models"_q), [=] {
		RefreshModels(controller);
	});
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
}

} // namespace LocalAi
