// Plazma port — applies the Plazma purple palette app-wide.
//
// Rather than painting hardcoded colors, this drives Telegram's own theme
// engine: it feeds the Plazma purple palette (dev/plazma/theme/
// plazma-purple.colors.txt) into Window::Theme::ApplyEditedPalette so the whole
// UI — feed, chat list, chats — turns purple the integrated Telegram way.
#pragma once

namespace Plazma {

// Applies the embedded purple palette live. Idempotent; safe to call once the
// window/theme machinery is up (e.g. from MainWidget construction).
void ApplyPlazmaPurpleTheme();

} // namespace Plazma
