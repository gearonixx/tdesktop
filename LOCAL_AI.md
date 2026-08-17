# Local AI mode

This fork replaces the people you talk to with the models a
[llama.cpp](https://github.com/ggml-org/llama.cpp) server is serving. Every
chat in the dialogs list is one model. You type, the model answers, the answer
streams into the bubble token by token with the same typewriter reveal Telegram
uses for streamed bot drafts.

Nothing about the chat path touches Telegram: in local mode no MTProto session
is opened at all, and the updater is switched off, so the only host the app
talks to is the llama.cpp server you point it at.

---

## 1. Run a server

```bash
./local-ai-server.sh setup                                   # clone + build llama.cpp
./local-ai-server.sh model unsloth/Qwen3-4B-Instruct-2507-GGUF '*Q4_K_M.gguf'
./local-ai-server.sh serve                                   # http://127.0.0.1:8080
```

`serve` with no arguments starts `llama-server` in **router mode** over
`~/llama-models`, so every `.gguf` in that directory shows up in `/v1/models`
and therefore as a chat in the app. Pass explicit paths to serve a single model
instead.

Useful knobs (all environment variables): `BACKEND` (`cuda`/`metal`/`vulkan`/
`cpu`, autodetected), `NGL`, `CTX`, `PARALLEL`, `MODELS_MAX`, `HOST`, `PORT`,
`API_KEY`.

### Renting a GPU

Run the server on the rented box and tunnel it, rather than exposing the port:

```bash
# on the GPU box
HOST=127.0.0.1 PORT=8080 NGL=999 ./local-ai-server.sh serve
# on your laptop
ssh -N -L 8080:127.0.0.1:8080 user@gpu-box
```

The app stays pointed at `http://127.0.0.1:8080` and does not need to know the
model moved. If you must bind `0.0.0.0`, set `API_KEY` and put the same value in
the app's settings — an open llama.cpp port lets anyone use your GPU and read
whatever you send it.

## 2. Point the app at it

Main menu (hamburger) → **Local AI settings**:

| Field | Meaning |
|---|---|
| Server | Base URL. `http://host:port`, with or without a trailing `/v1`. |
| API key | Sent as `Authorization: Bearer …` when set. |
| System prompt | Prepended to every conversation. |
| Temperature, Top P | Sampling, passed straight through. |
| Answer limit | `max_tokens`; 0 leaves it to the server. |
| Messages kept as context | How much history is replayed on each turn. |
| Your name | The name on your own messages. |
| Local AI mode | Off + restart turns this back into a normal Telegram client. |

**Refresh model list** asks the server for `/v1/models` and creates a chat for
every model it did not know yet. It also runs automatically at startup.

## 3. Using it

- Send a message — the answer streams in, with a spinner emoji at the tail
  while it is still being written and a "typing" indicator in the header.
- Markdown from the model is rendered: `**bold**`, `` `code` ``, fenced blocks
  with syntax highlighting once the answer is complete.
- Chat menu (⋮) → **Stop generating** while an answer is running,
  **Regenerate answer** to drop the last answer and ask again.
- **Clear history** empties the transcript. **Delete chat** also hides the model
  until you refresh the model list again.
- Transcripts live in `tdata/local_ai/chat-<peer>.jsonl`, one JSON object per
  message; settings in `tdata/local_ai/config.json`. Delete either to start over.

---

## How it works

The point of the design is to reuse Telegram Desktop's chat UI verbatim — the
dialogs list, the bubbles, the composer, notifications, search — and to swap out
only what is underneath.

| Concern | Where |
|---|---|
| Config + transcript storage | `SourceFiles/local_ai/local_ai_config.cpp`, `local_ai_store.cpp` |
| HTTP + SSE client for llama.cpp | `SourceFiles/local_ai/local_ai_client.cpp` |
| Peers, sending, streaming | `SourceFiles/local_ai/local_ai_chats.cpp` |
| Synthetic session (no sign-in) | `SourceFiles/local_ai/local_ai_bootstrap.cpp` |
| Settings UI | `SourceFiles/local_ai/local_ai_settings.cpp` |

Five hooks connect it to the rest of the app:

1. **`Main::Domain::activateAfterStarting`** calls
   `LocalAi::CreateLocalSession()`. An unauthorized account gets a synthetic
   self user and a `Main::Session` immediately, so the intro/sign-in flow never
   appears.
2. **`Main::Session` constructor** calls `Chats::bootstrap()`, which turns each
   known model into a `UserData` (via a locally built `MTPUser`, marked as a
   bot), replays its transcript into the `History` with `addOlderSlice()`, and
   calls `chatsListDone()` so the dialogs list is not stuck loading.
3. **`ApiWrap::sendMessage`** defers to `Chats::trySend()` before anything else,
   mirroring how the upstream ephemeral-messages component intercepts sends.
   The user's message is added as a real history item, then a placeholder answer
   is created with `MessageFlag::TextAppearing` and filled in from the stream.
4. **`ApiWrap::deleteHistory`** routes clear/delete for model chats to the local
   transcript instead of the server.
5. **`MTP::Instance`** opens no session and drops every request in local mode.
   This is the load-bearing part: creating an MTProto session would dial
   Telegram to negotiate an auth key, and a real reply to, say, `getDialogs`
   with an unauthorized key would answer `401` and destroy the synthetic
   session.

Messages are ordinary server-shaped messages (not "local" ones), with ids from
a single counter persisted in `config.json`, because `Data::Session` keeps one
non-channel message map for the whole account and ids must be globally unique.
Making them regular is what keeps reply, forward, select and pin working.

### Known limits

- Deleting individual messages hides them until restart; only *Clear history*
  and *Delete chat* are written back to the transcript.
- One server at a time. Multiple backends means one llama.cpp router (or a
  swap proxy) in front of them.
- Attachments, voice, calls and everything else cloud-shaped is inert — the
  requests are dropped rather than erroring, so those buttons simply do nothing.
- `**bold**`, `` `code` ``, fences and `~~strike~~` are understood; single-`*`
  emphasis and `#` headings are not (Telegram's markdown does not have them).
