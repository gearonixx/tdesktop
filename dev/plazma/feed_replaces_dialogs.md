# "Open Telegram → get a video feed": replacing the messaging plane

The user's framing: authorization stays exactly Telegram's; but the moment you
finish login, instead of the **dialogs list + chat history**, you land in a
**video feed** — same interaction model, playlists, but leaning into
Telegram-native constructs for the "advanced" bits.

## tdesktop's window anatomy (what we're repurposing)

After `intro/` (login) completes, `Window::SessionController` shows the main
UI, which is a `Window::MainWidget` split into columns:

- **Left rail** — `Ui::MainMenu` / the accounts + folders side bar
  (`SourceFiles/window/`, `dialogs/` filters).
- **Dialogs column** — `Dialogs::Widget` (the scrollable chat list + search).
- **Third column** — the active *section*: `HistoryWidget` (a chat), or a
  `Window::SectionWidget` subclass (Settings, Info, Calls list, etc.), swapped
  via `SessionController::showSection(std::make_shared<...Memento>())`.

Everything the user calls "the messaging feed" is those middle+right columns.
Crucially, **sections are already a first-class, swappable abstraction** — that
is the seam we exploit.

## The swap, in three escalating steps

### Step 1 — coexistence (always-green): Plazma as a section
Implement the feed as a `Window::SectionWidget` (`PlazmaSection`) with its
`SectionMemento`. Add an entry point in the left rail (like "Saved Messages" or
"Calls"). `showSection(PlazmaMemento())` puts the feed in the third column. The
messaging UI is untouched → the build always runs. This is where we validate the
feed list, cards, and the `MpvWidget` watch page.

### Step 2 — feed *is* the dialogs column
Replace the content of the **dialogs column** so the primary list is not chats
but the **video feed / creators list**. Two sub-options:

- **2a (fast):** keep `Dialogs::Widget`'s chrome (search bar, folder tabs,
  scroll, ripple rows) and swap its **row model** — render `VideoItem` rows
  (thumbnail + title + author + duration) instead of `Dialogs::Row`. Reuses
  tdesktop's virtualized list, search field, and folder tab bar for free.
- **2b (clean):** a purpose-built `Plazma::FeedList : Ui::RpWidget` with a card
  grid (YouTube-like), keeping only the search field + folder tabs from
  tdesktop. More layout freedom, less free machinery.

Recommend **2a first** (reuses the most, ships fastest), migrate hot rows to 2b
cards later. Clicking a row → `showSection(PlazmaWatchMemento(video))` → the
third column becomes the `MpvWidget` watch page.

### Step 3 — make it the landing surface
Change the post-login default from "show dialogs / last chat" to
"`showSection(PlazmaMemento())`", and gate the messaging entry points behind a
flag (Settings toggle or build define) so messaging is *reachable* but not the
face of the app. Now: **open the app → log into Telegram → video feed.** Later,
Phase 7 rebrands (name/icon/updater feed).

## Authorization — unchanged, bridged once

Login is stock tdesktop `intro/` → real MTProto session. **No TDLib.** On
`Main::Session` ready, bridge identity to PlazmaServer once:

```cpp
// after session ready:
const auto self = session->user();
auto login = Plazma::UserLogin{
    .userId      = peerToUser(self->id).bare,
    .username    = self->username(),
    .firstName   = self->firstName,
    .lastName    = self->lastName,
    .phoneNumber = self->phone(),
    .isPremium   = self->isPremium(),
};
plazmaApi->loginUser(login); // → bearer token for the feed
```

Same fields Plazma sent from TDLib's `authorizationStateReady`, now sourced from
the session tdesktop already established. One function, zero extra auth UI.

## Playlists → Telegram-native "advanced" mapping

This is where "same functionality, but Telegram-style" pays off. Map Plazma's
playlist concepts onto machinery tdesktop already ships:

| Plazma concept | Telegram-native reuse in tdesktop | Advanced twist |
|---|---|---|
| Playlist list | **Chat folders / dialog filters** (the top tab bar) | Each folder = a playlist; the existing folder editor UI edits playlists. |
| "Saved" playlist | **Saved Messages** analog | "Watch later" = your personal saved feed. |
| Share a video | **Forward / share box** (`Window::ShowShareBox`) | Share a `VideoItem` link into any real Telegram chat. |
| Playlist collaboration | **Group chat** backing a playlist | A shared playlist is a group; members add items. |
| Creator / channel | **Channel subscribe** model | "Following" feed = creators you subscribe to, like channel subscriptions. |
| Comments on a video | **Discussion replies** UI | Reuse the reply thread widget under the watch page. |
| Reactions | tdesktop **reactions** picker | Like/emoji-react to a video with the same picker. |
| Search | `Dialogs` **search field** + server `?q=` | Same debounce (~250ms) Plazma already tuned. |

Concretely for the MVP: playlists ride on **dialog filters (folders)** — the
folder tab bar becomes the playlist switcher, and the folder edit box becomes
"edit playlist". That reuses a large, polished chunk of tdesktop UI verbatim and
instantly gives the "Telegram-style" advanced feel the user wants, while the
authoritative playlist data still lives in PlazmaServer
(`/v1/playlists`, see `PlazmaServer/docs/playlists.md`).

## Build order (maps to report phases)

1. `PlazmaSection` + `PlazmaMemento` (Step 1) — feed list + `MpvWidget` watch.
2. Session→login bridge (above).
3. Swap dialogs-column row model to `VideoItem` (Step 2a).
4. Playlists on dialog filters; share/forward reuse.
5. Flip landing surface to the feed (Step 3).

## Risks specific to this plane

- `Dialogs::Widget` is deeply tied to `Data::Session` chat data; **2a** means
  either subclassing its row provider or feeding it synthetic entries. If that
  coupling bites, fall back to **2b** (own list) — more code, fewer surprises.
- Folders (dialog filters) are premium-gated and server-synced in real Telegram;
  for playlists we drive them locally against PlazmaServer, so we must
  **decouple the folder UI from the MTProto filter sync** or reimplement a
  lightweight tab bar with the same look.
