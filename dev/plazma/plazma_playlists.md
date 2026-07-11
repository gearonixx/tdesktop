# Playlists as Telegram folders (chat filters)

The user's steer: **playlists should be "folder"-level functionality — like
Telegram's chat folders / filters, but over videos.** Telegram Desktop already
ships a complete, polished folders subsystem; we reuse it as the playlist UI and
keep PlazmaServer as the source of truth for playlist data.

## The tdesktop machinery we reuse (verified in-tree)

| Concern | tdesktop file / type | Reused as |
|---|---|---|
| A folder definition | `data/data_chat_filters.h` — `ChatFilter` (`FilterId id()`, `ChatFilterTitle title()`, `Flags flags()`, included/excluded peer lists) | A **playlist**: `id` ↔ playlist id, `title` ↔ playlist name |
| The folder collection | `data/data_chat_filters.h` — `ChatFilters` (list, reorder, CRUD, change signals) | The **playlist set** for the logged-in user |
| The left-rail folder strip | `window/window_filters_menu.{h,cpp}` | The **playlist switcher** (vertical tabs, icons, reorder, "+ add") |
| Folder icons | `dialogs/ui/filter_icons.*`, `filter_icon_panel.*` | Playlist icons / cover glyphs |
| Folder editor box | the "Edit folder" box (includes/excludes UI) | The **"Edit playlist"** box (rename, add/remove videos) |

The strip and editor are generic enough that the *look and interactions* — tabs,
active highlight (now purple), reorder drag, context menu, the "All chats"
default tab — all come for free.

## Two integration strategies

### Strategy 1 — Reuse `ChatFilters` as the model (deepest reuse, more coupling)
Feed PlazmaServer playlists into the existing `Data::ChatFilters` as synthetic
filters, so `window_filters_menu` renders them unchanged.

- On `Api::listPlaylists`, build a `ChatFilter` per `Plazma::Playlist`
  (`FilterId` from a stable hash of the playlist id; `title` = name).
- The filter's "included chats" list is repurposed to hold the playlist's video
  ids (we intercept selection to show videos, not a dialogs list).
- **Cost:** `ChatFilters` is wired to MTProto filter *sync* (`updateDialogFilters`).
  We must **sever the server-sync path** for our synthetic filters or it'll try
  to push them to Telegram. Doable but invasive — this is the "insanely full
  rewrite" surface the user referenced: the folders subsystem gets forked to
  drive PlazmaServer instead of MTProto.

### Strategy 2 — Own model, reuse the *view* (recommended first)
Keep a `Plazma::PlaylistsModel` (port of Plazma's `PlaylistsModel`, which already
holds the client-generated UUID-v7 → synchronous `createPlaylist(name)→id`
contract) and render it with a **copy of `window_filters_menu`'s widget** pointed
at our model.

- No fight with MTProto filter sync — our tabs are purely local + PlazmaServer.
- Reuse the styling/interaction code by parameterizing the menu widget over a
  data source interface instead of hard `Data::ChatFilters`.
- Clicking a playlist tab → `showSection(PlazmaFeedMemento{playlistId})`; the
  feed list then shows that playlist's items (or the global feed for the "All"
  tab).

Recommend **Strategy 2** to ship, then optionally deepen to Strategy 1 once the
folder subsystem is forked for good (Phase 6, when messaging is being removed
anyway).

## "Filters for folders" — the advanced twist

Beyond playlists-as-folders, folders can carry **filter rules over the video
feed** (the user's "filters for folders" phrasing), mirroring how Telegram
folders filter chats by type/unread/etc. Concrete video analogues:

- **By source:** creator/channel you follow, your uploads, saved/watch-later.
- **By state:** unwatched, in-progress (resume), downloaded-offline.
- **By kind:** codec/quality (e.g. AV1/4K), duration buckets, mime.
- **By tag/search:** a saved `?q=` query becomes a "smart playlist" folder.

Model it exactly like `ChatFilter::Flags`: a `Plazma::PlaylistFilter { Flags
flags; QString savedQuery; }`. A folder is then either a **manual playlist**
(explicit item list, server-backed) or a **smart folder** (a filter rule
evaluated client-side over the feed) — same tab strip, two backing kinds. This
is the "advanced, Telegram-style" behavior the user wants and it maps cleanly
onto the flags model tdesktop already uses.

## Data flow

```
PlazmaServer /v1/playlists  ──listPlaylists──▶ Plazma::PlaylistsModel
                                                     │
                             window_filters_menu (view, purple)  ◀── binds
                                                     │ tab click
                                                     ▼
                          showSection(PlazmaFeedMemento{playlistId|filter})
                                                     │
       manual playlist → items from the playlist    │  smart folder → feed
       (GET items)                                   ▼  filtered by flags/?q=
                                          Plazma feed list (VideoItem cards)
```

## Port status

- **API layer: DONE** — `plazma_api.{h,cpp}` now has `Playlist`/`PlaylistItem`
  types and the full CRUD (`listPlaylists`, `createPlaylist`, `renamePlaylist`,
  `deletePlaylist`, `addPlaylistItem`, `removePlaylistItem`), faithful to
  Plazma's contract (client-supplied id on create; `item`+`playlist` reply on
  add). Variable-id paths go through the new `Api::requestPath` helper (the
  port's `buildPlaylistRequest`).
- **Model: DONE** — `plazma_playlists.{h,cpp}` (`Plazma::Playlists`) ports
  `PlaylistsModel` around tdesktop idioms: rpl outputs instead of Qt signals,
  `crl::guard` on every async reply, optimistic-create with client-generated id
  (synchronous `create(name)→id`), rollback on server failure, and a
  videoId→playlistIds membership index fed by mutations plus the
  `GET .../by_video` endpoint (`ensureMembership`) so saved-state checks don't
  depend on lazily-loaded items. Detail hydration via `openPlaylist` →
  `Api::getPlaylist`. Wired into `Telegram/CMakeLists.txt`; compiles and links
  in the Debug container build.
- **View: pending** — parameterize a copy of `window_filters_menu` over the
  model (Strategy 2).
- **Smart folders: design above**, pending.
