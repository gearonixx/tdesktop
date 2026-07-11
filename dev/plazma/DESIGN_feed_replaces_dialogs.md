# Design: the dialogs list becomes the video feed (Telegram-native)

Iteration #1 addendum, agent 2. Answers the concrete question: *when you open
Telegram, instead of the chat list you see a video feed — same shell, same
login — with playlists done the Telegram way.* All file/line refs are in
`/home/x/c/tdesktop_2/Telegram/SourceFiles`, branch `feature/plazma-mvp`.

## The two-column shell — where messaging lives, and the exact swap points

tdesktop's main screen is a two-column `MainWidget`:

- **Left column = the chat list.** Built at `mainwidget.cpp:264` as
  `base::make_unique_q<Dialogs::Widget>(this, controller, Dialogs::Widget::Layout::Main)`,
  held as `const base::unique_qptr<Dialogs::Widget> _dialogs;`
  (`mainwidget.h:344`). Internally it's a scroll of `Dialogs::Row`s
  (`dialogs/dialogs_inner_widget.cpp`, `dialogs_row.cpp`, `dialogs_list.cpp`).
- **Right column = the open chat** (`HistoryWidget`) or a stacked
  `Window::SectionWidget` shown via `Window::SessionController::showSection`.

So "video feed instead of messaging" is two concrete replacements:

1. **Left column:** swap the `Dialogs::Row` content for **video-card rows** — the
   feed. Two ways to do it, cheapest first:
   - **B0 (recommended MVP): a sibling widget.** Add
     `Plazma::FeedWidget : Ui::RpWidget` (own `Ui::ScrollArea` of cards) and put
     it where `_dialogs` goes — either replace `_dialogs` outright, or add it as
     a second stacked child of `MainWidget` toggled by a flag so messaging still
     builds. Feed rows are painted cards (thumbnail + title + author), fed by
     `Plazma::Api::fetchVideos("")`. This touches one file (`mainwidget.cpp`) and
     reuses nothing fragile.
   - **B1 (later, tighter): reuse the dialogs engine.** `Dialogs::Row` /
     `Dialogs::InnerWidget` are already a virtualized, searchable, pinnable,
     reorderable list. Teaching it a "video entry" row type gives search,
     keyboard nav, context menus, and the pinned section *for free*. More work,
     but it's how you get the *"same functionality"* feel without rebuilding it.
2. **Right column:** clicking a card opens a **watch surface**
   (`Plazma::WatchSection : Window::SectionWidget`) through the existing
   `showSection` path — exactly how Telegram opens a chat, so back/forward,
   layers, and the media viewer all keep working.

## Auth — unchanged, and it's already done

Same as the user's instinct: identical login. tdesktop finishes its real
Telegram `intro/` MTProto login, then `Plazma::Backend`
(`plazma/plazma_session.*`, added this iteration) reads `Main::Session::user()`
and POSTs `{user_id, username, first_name, last_name, phone_number, is_premium}`
to `/v1/auth/login` for the bearer token. No TDLib, no second client, no second
login screen — you sign into Telegram, the feed is already yours.

## Playlists, the Telegram-native way (the "advanced" part)

Plazma's playlists are a flat CRUD list. Telegram Desktop already ships the
perfect richer paradigm: **chat folders**. Reuse it instead of reinventing:

- `data/data_chat_filters.h` — `ChatFilter` / `ChatFilters` (`:39`, `:149`): the
  model behind foldered, tabbed, orderable collections.
- `window/window_filters_menu.*` — the left-rail folder tabs UI.

Map **playlist → folder-style tab**. Then playlists inherit, for free, the
Telegram interactions users already know:

- The **folder rail / tab bar** across the top of the feed = your playlists
  ("Watch later", "Music", "Liked"), reorderable by drag, same as chat folders.
- **Pin a video** inside a playlist = the dialogs **pinned** section
  (`dialogs_pinned_list.cpp`) — already reorderable.
- **Add to playlist** = the same context-menu affordance as "Add to folder"
  (`window/window_peer_menu.cpp` is the pattern to copy).
- **Search within feed/playlist** = the dialogs search field is already there.
- Server side, playlists already exist (`PlazmaServer/docs/playlists.md`,
  `Plazma::Api` playlist CRUD — port next); the *client* just renders them as
  folders instead of a plain list.

This is what makes it feel like "YouTube built by the Telegram team" rather than
a webview: playlists behave like folders, the feed behaves like the chat list,
the watch page behaves like an opened chat.

## Concrete build order (each step keeps the tree compiling)

1. **[done]** `plazma_api.*` (backend) + `plazma_session.*` (auth bridge), wired
   into `Telegram/CMakeLists.txt`; purple `.tdesktop-theme`.
2. **[done]** `Plazma::FeedWidget` (B0) — `Ui::RpWidget` + `Ui::ScrollArea`
   card scroll (`plazma_feed_widget.{h,cpp}`). Owns its own `Plazma::Backend`,
   logs in with the session, calls `fetchVideos("")`, paints title/author/date
   cards with a thumbnail placeholder + "Plazma" header + status line.
   `MainWidget` constructs it next to `_dialogs` and positions it over the
   chat-list column in both one- and two-column layouts (see the `_plazmaFeed`
   edits in `mainwidget.{cpp,h}`). Builds + links + runs. Shows a
   "start the backend on localhost:8080" status until PlazmaServer is up.
   *Follow-ups:* real thumbnails need an HTTP image loader; click-to-open a
   watch surface is step 3.
3. `Plazma::WatchSection : Window::SectionWidget` — opened via `showSection`.
   **De-risk playback first (see SESSION_FINDINGS gotcha #2):** `Media::Streaming`
   has only local + MTProto loaders, no HTTP one — prototype an HTTP
   `Streaming::Loader` (wrap ranged GETs, model on `Plazma::Api::startDownload` +
   `part_file`) or keep libmpv behind a flag.
4. Playlists as folder-style tabs (reuse `ChatFilters` + `window_filters_menu`
   patterns), backed by the ported playlist API.
5. Flip the feed to the default left-column surface; hide messaging behind a
   flag (Approach B). Then rebrand (Phase 7, mind `LEGAL`).
