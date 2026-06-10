# Offline Notes mode

Turns Telegram Desktop into a single, nameless, fully-offline chat — a personal
note log in the spirit of "Saved Messages", but with no account, no network, no
proxy, and no metadata beyond the messages themselves. Notes live on disk as
human-readable Markdown in a folder, so they're editable by hand and compatible
with tools like Obsidian.

This is built in-place on a branch (`offline-notes`), not as a separate app: we
keep Telegram's message UI (bubbles, media, pins, edits, reactions) and replace
the data/transport layer underneath it.

## Milestones

- **M1 — Local Markdown storage backend** (done): `storage/offline/` reads and
  writes the chat as `messages.md` + a `media/` folder. Self-contained and unit
  tested.
- **M2 — Offline boot**: construct an authorized `Main::Account`/`Session` with a
  synthesized self user, skipping the intro/login flow; persist it so startup is
  instant.
- **M3 — Network neutralization**: MTProto never connects; `ApiWrap` sends become
  local-only; the updates loop and all connection/proxy UI are disabled.
- **M4 — Single-chat UI**: open straight into one nameless chat; hide the dialog
  list, account switcher, settings, and peer name/avatar/status.
- **M5 — Persistence hooks**: intercept send / edit / delete / pin / react /
  attach to build local `HistoryItem`s (`History::addNewLocalMessage`) and persist
  through M1; load all notes into the `History` on startup.

## On-disk format (M1)

A notes folder contains:

```
messages.md   all notes, chronological, fully round-trippable
media/        imported photos / videos / files referenced by notes
```

Each note is a CommonMark body wrapped in an HTML comment carrying its metadata.
HTML comments render invisibly in any Markdown viewer, so the file reads cleanly
in Obsidian while staying losslessly parseable:

```
<!--note id="1749550000123-0000" date="2026-06-10T12:00:00Z" pinned="1"
         reactions="❤️:3,👍:1" media="media/photo_ab12.jpg"-->
Message body in **CommonMark** with a [link](https://example.com).
<!--/note-->
```

Metadata fields: `id` (stable, sortable), `date` / `edited` (ISO-8601 UTC),
`pinned`, `reactions` (`emoji:count` list), `media` (`|`-separated relative
paths, with `|`/`%` percent-escaped). Bodies are arbitrary text; a literal
`<!--/note-->` inside a body is escaped on write. Writes are atomic
(`QSaveFile`); the store is rewritten whole on each change.

See `storage/offline/offline_notes_storage.h` for the API
(`load` / `append` / `update` / `remove` / `importMedia`).
