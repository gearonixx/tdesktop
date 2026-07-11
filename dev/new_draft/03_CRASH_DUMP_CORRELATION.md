# Real-Client Crash-Dump Correlation

This ties the fuzzing findings to the **actual Telegram Desktop process** via its
Breakpad minidumps, and is the evidence that `\kern` is a control-flow hijack in the
shipping client rather than a sanitizer artifact.

Dumps: 100 total across `~/dumps`, `~/.local/share/TelegramDesktop/tdata/dumps`,
`~/tgreport/raw`, spanning **2026-03-14 → 2026-07-03**.
telegram-desktop module range: **`0x555555554000 – 0x55555d561000`**.

---

## 1. The control-flow signature is unique and new

Across all 100 dumps, **exactly 4** have `rip == fault address` with `rip` **outside any
loaded module** (i.e. control transferred to a non-code address). All 4 are from
**2026-07-03**, in two clusters:

| Dump | Time | `rip` (= fault) | In module? |
|---|---|---|---|
| `a6c07f72-…` | 07:38:30 | `0x555570455100` | no — heap |
| `83cf7b01-…` | 07:39:48 | `0x555573213180` | no — heap |
| `71aaf1b3-…` | 08:19:06 | `0x55556ee671a0` | no — heap |
| `e33d16a3-…` | 08:20:18 | `0x555574ca9e00` | no — heap |

The other **96** dumps (including the 89 from the prior triage) all crash with `rip` in
valid code — controlled `abort()` or null-deref → **safe DoS**. The 07:38–08:20 window
is the interactive testing session.

**Target varies every run** (`0x55556e…`, `0x555570…`, `0x555573…`, `0x555574…`) — all
above the binary, in the heap. A target read from memory that moves with heap layout is
the signature of an **indirect call through a corrupted/OOB pointer**, not a fixed code path.

## 2. Live reproduction nails attribution

`71aaf1b3` (08:19) and `e33d16a3` (08:20) were produced **on demand by pasting `\kern`**
into the running client. They match the two earlier "mystery" dumps
(`a6c07f72`, `83cf7b01`) in crash reason, faulting instruction, register file, and caller
chain. Therefore the mystery dumps were `\kern` as well, and the behavior is
**deterministic and software-induced**. (Deterministic on-demand reproduction also
**falsifies** any thermal/bit-flip explanation, despite the stackwalker's "possible
flipped bit" heuristic note.)

## 3. Register file — `e33d16a3` (live `\kern`), crashing thread

```
rip = 0x0000555574ca9e00      <-- the CALL TARGET (varies per run; heap)
rdi = 0x00005555625ff060      <-- CONSTANT across all 4 dumps: &_M_functor
                                  (address of the std::function object at
                                   _unitConversions[-1] — a fixed global)
rsi = 0x0000555564242f70      <-- the Environment& env argument (varies; heap)
rax = 0xffffffffffffffe0
rcx = 0x0000555563994280      <-- constant across dumps
r8  = 0x0000000000000000      <-- constant
r9  = 0x000055556236a7b8      <-- constant
r10 = 0x000055556246ec90      <-- constant
r14 = 0x00005555625ff080      <-- constant (= rdi + 0x20)
rbp = 0x00007fffffffb350   rsp = 0x00007fffffffb308
```

**Interpretation (libstdc++ `std::function` call ABI).**
`std::function::operator()` compiles to `return _M_invoker(_M_functor, args…)`. The
layout is `{ _Any_data _M_functor [16] ; _Manager_type _M_manager [8] ; _Invoker_type
_M_invoker [8] }` (32 bytes). So:

- `rdi = &_M_functor` — the address of the `std::function` object being invoked. It is
  **constant = `0x5555625ff060`** in every dump ⇒ a fixed global. This global is
  `_unitConversions[-1]` = 32 bytes before the real array.
- `rsi = env` — the second argument, the live `Environment&`.
- `rip = _M_invoker` — the function pointer read from `[_unitConversions[-1] + 0x18]`.
  It is a **heap** value that **varies per run** ⇒ the global's bytes, reinterpreted as a
  `std::function`, contain a heap pointer in the invoker slot; the code jumps there.

The constant `rdi`/`rcx`/`r8`/`r9`/`r10` and varying `rsi`/`rip` across four independent
crashes are exactly what a deterministic indirect call `_M_invoker(&globalFunctor, env)`
produces — not random corruption.

## 4. Call-chain reconstruction (all 4 `\kern` dumps, identical)

```
#0  0x5555_74ca_9e00            <-- wild heap target (the hijacked rip)
#1  telegram-desktop + 0x53d2677  \  the MicroTeX render walk:
#2  telegram-desktop + 0xb77e11    )  SpaceAtom::createBox → getFactor (indirect call)
#3  telegram-desktop + 0xb79fe5    )  ← RowAtom/Dummy::createBox
#4  telegram-desktop + 0xb883c3   /   ← TeXRenderBuilder::build
#5  telegram-desktop + 0x2f1f779
#6  telegram-desktop + 0x2f21670
#7  telegram-desktop + 0x2fcd672
```

The **same sub-chain `+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3`** appears in the
provably-MicroTeX dumps from the same session:

| Dump | Class (matched to a gdb-characterized bug) | Shares MicroTeX sub-chain? |
|---|---|---|
| `9fb12b82` (07:40) | `SIGABRT`, `_GLIBCXX_ASSERTIONS` `__n < size()` `[vector<shared_ptr<Atom>>]` = **N4** | yes (`…+0x53d2677`) |
| `3f1b0d71` (07:39) | `SIGSEGV`, page-boundary fault, `rip` valid = **N2** | (MicroTeX render frames) |
| `cc4c56bb` (07:43) | `SIGSEGV @0x0`, `rip` valid = **multirow / null-cluster** | yes (`…+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3`) |

So the 5-dump testing cluster decomposes cleanly:

- 2× **N4** signature is *provably* MicroTeX (the assertion text names the element type).
- 1× **N2**, 1× **multirow/null** by signature.
- 2× (then +2 live) `\kern` **control-flow** dumps sharing the identical MicroTeX render
  caller chain, differing only in that at `+0x53d2677` control leaves for a wild heap
  address instead of descending into a valid `createBox`.

That shared caller chain is the concrete link tying the wild-`rip` crashes to the
MicroTeX `SpaceAtom` render path — i.e. to `\kern` / `getFactor`.

## 5. Faulting instruction (post-hijack)

`minidump-stackwalk` decodes the instruction at the hijacked `rip` as, e.g.,
`mov al, byte [0x10000555561fb26]`, accessing a **non-canonical** address → `SIGSEGV /
SEGV_ACCERR`. That is simply *whatever the heap bytes at the jump target happened to
decode to*; it is the consequence of the hijack, not the bug itself. With different heap
contents the executed bytes — and thus the outcome — differ. This is precisely why the
finding is a control-flow-hijack **primitive** whose ultimate impact depends on heap state.

## 6. Not related: the older 89 dumps

The pre-existing 89-dump triage (`dev/crash-triage/`) found **0** MicroTeX crashes and
**0** control-flow signatures — a separate `std::vector<Dialogs::Key>::operator[]`
bounds-`abort()` + null-deref cluster in the chat-list code, all safe DoS. The `\kern`
signature is genuinely new and first appears on 2026-07-03.
