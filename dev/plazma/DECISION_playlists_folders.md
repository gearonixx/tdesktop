# Playlists as "video folders" — approaches, decision, MVP

Iteration #2, agent 2. Implements the playlist *logic* as real, bodied,
cmake-wired code (not doc-only). Answers "look at how Plazma does playlists and
implement it, mapped to Telegram-style folders."

## How Plazma implements playlists (studied in source)

`Plazma/src/models/playlists_model.{h,cpp}` (177 + 642 LoC). The contract:

- **Single source of truth = the server** (`PlazmaServer/docs/playlists.md`); the
  model keeps a **local mirror** only so QML can read synchronously. It
  deliberately does **not** persist to QSettings — a stale cache after another
  device mutated state is worse than a brief load.
- **Optimistic + reconcile** on every mutation: (1) apply locally + emit change
  signals so the UI updates instantly, (2) fire the Api call, (3) on 2xx
  reconcile server-canonical fields, (4) on 4xx/5xx **roll back** and surface an
  error.
- **Synchronous `createPlaylist(name) → id`:** the id is generated *client-side*
  (UUID v7-shaped) and registered with the server in the background, so callers
  get an id immediately.
- Query helpers for the "Save to playlist" menu: `playlistContains`,
  `playlistsContaining(videoId)`, `summariesForVideo`.
- Ordering: case-insensitive by name, tie-broken by id.

## The mapping question: what is a "Telegram folder", really?

I read tdesktop's folder machinery before choosing:

- `data/data_chat_filters.h` — `ChatFilter` is **peer/history centric**:
  `contains(not_null<History*>)`, include/exclude **peer** lists, peer-type
  flags. Its whole job is *filtering the chat list by peer*.
- `ui/widgets/chat_filters_tabs_strip.h` — `AddChatFiltersTabsStrip(parent,
  session, choose, …)` is **welded to `Main::Session` + `FilterId`**; it reads
  the session's real filters and emits a `FilterId`. Not a generic tab bar.

**Videos are not peers/histories.** So the two obvious "reuse it" ideas are traps.

## Approaches considered

### A — Reuse `Data::ChatFilters` literally (one ChatFilter per playlist)
Make each playlist a real chat filter. **Rejected.** You'd have to fake a
`History`/peer per video to satisfy `ChatFilter::contains`, fight the
include/exclude-peer data model, and `AddChatFiltersTabsStrip` would still want
real `FilterId`s from the session. Wrong abstraction; high friction; fragile.

### B — Purpose-built model + borrow the folder-tab *look* (CHOSEN)
Keep a dedicated `Plazma::Playlists` controller (this MVP) as the data model —
a straight port of Plazma's optimistic-mirror logic. For UI, render playlists as
a **folder-style tab strip over the feed**, visually modeled on
`chat_filters_tabs_strip` but backed by our controller, emitting a *playlist id*
instead of a `FilterId`. Selecting a tab filters the feed to that playlist's
items; the "All" tab is the full feed.
- **Pros:** correct data model; the Telegram-native *interaction* (folder tabs,
  reorder, active highlight) without abusing peer types; the controller is
  testable in isolation and already compiles as a unit.
- **Cons:** the tab-strip widget is a (small) new widget rather than a literal
  reuse — but that's unavoidable given the type mismatch.

### C — Flat list page (like Plazma's `PagePlaylists`)
A plain list/grid of playlists, no tabs. **Fallback.** Simplest, but it's "a
playlists screen", not the "folders over the feed" feel the user asked for.
Good as a secondary management screen; not the primary surface.

## Decision

**B.** Data = `Plazma::Playlists` (delivered here). UI = a folder-tab strip
lookalike bound to it (next brick). Do **not** route playlist data through
`Data::ChatFilters`.

## MVP delivered this iteration (real code, cmake-wired)

`Telegram/SourceFiles/plazma/plazma_playlists.{h,cpp}` — `Plazma::Playlists`,
a `QObject` controller:

- Local mirror `std::vector<Playlist>` over the `Plazma::Api` playlist methods
  (`listPlaylists/createPlaylist/renamePlaylist/deletePlaylist/addPlaylistItem/
  removePlaylistItem`, already ported into `plazma_api.*`).
- Faithful **optimistic + reconcile** for every mutation, including rollback on
  failure (`create`/`rename`/`remove`/`addVideo`/`removeVideo`).
- Synchronous `create(name) → id` with a client-generated `QUuid` id, exactly
  like Plazma.
- Query helpers `contains`, `playlistsContaining(videoId)` for the
  "Add to playlist" affordance; `isValidName`/`isNameTaken` validation.
- `refresh()` (coalesced) to pull the server snapshot; `changed()` /
  `errorOccurred(QString)` signals to drive the UI.
- Wired into `Telegram/CMakeLists.txt` (`plazma/plazma_playlists.cpp/.h`).

Honest scope: this is the **model/logic**, bodied and reviewable. Not yet built
(the tree hasn't been compiled), and there is **no UI yet** — the folder-tab
strip and the feed-filter binding are the next brick.

## "Advanced, Telegram-style" extensions (once the tab strip exists)
Each maps a known Telegram interaction onto the folder-backed feed:
- **Folder tab bar over the feed** = playlist switcher (reorderable, like folders).
- **Pin a video** in a playlist = the dialogs **pinned** section pattern.
- **Add to playlist** = the "Add to folder" context-menu affordance
  (`window/window_peer_menu.cpp` is the pattern to copy).
- **"Watch later"** = a Saved-Messages-style default folder.
- **Share a video** = the forward/share box.

## Next bricks
1. `plazma_playlists_tabs.{h,cpp}` — the folder-tab `Ui::RpWidget` strip bound to
   `Plazma::Playlists::changed()`, emitting the selected playlist id.
2. Feed filter: when a playlist tab is active, show `Playlist::items` instead of
   the full `fetchVideos` feed.
3. "Add to playlist" menu on a feed card → `Playlists::addVideo`.
4. Compile the `Telegram` target and fix the first round of in-tree errors.
