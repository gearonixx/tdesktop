# Methodology

Everything below is reproducible on the machine as configured. Paths are literal.

## 1. Builds used

MicroTeX source: `/home/x/c/tdesktop/Telegram/ThirdParty/MicroTeX/src`
(debug path baked into binaries: `/home/x/c/tdesktop/Telegram/ThirdParty/MicroTeX/...`).

| Build | Path | Sanitizer | Input | Purpose |
|---|---|---|---|---|
| ASan argv | `dev/microtex-fuzz/build_a/plain` | ASan | `argv[1]` | fast triage, ASan reports |
| **Release argv** | `dev/microtex-fuzz/build_plain/plain` | none (`-DNDEBUG`, `_GLIBCXX_ASSERTIONS`) | `argv[1]` | **true faulting behavior** |
| Release file | built this session → `scratchpad/rel_file` | none | file (exact bytes, incl. NUL) | PoCs with NUL / high bytes |
| AFL non-persistent | `dev/microtex-fuzz/build_afl2/afl_microtex_np` | ASan+UBSan (vptr off) | file | crash-corpus replay |

**Why the release build matters.** ASan inserts redzones and intercepts allocation, so
an out-of-bounds *read* faults inside ASan *before* downstream code runs. That masks
what the shipping binary actually does. Every "shape" verdict here (data-fault vs
control-flow-transfer) was taken from the **release** build, then corroborated against
real crash dumps.

Sanity control: the release harness must reproduce a known crash (`\kern` → SIGSEGV) and
the ASan build must flag a known bug, on every sweep, so an "OK" result is trustworthy.

## 2. gdb procedure (the core of this report)

For each PoC, on the **release** build:

```
gdb -q -batch \
  -ex 'set debuginfod enabled off' \
  -ex run \
  -ex 'bt'                                  # full call chain
  -ex 'info symbol $rip'                    # is rip in valid code?
  -ex 'x/i $rip'                            # the faulting instruction
  -ex 'print $_siginfo._sifields._sigfault.si_addr'   # fault address
  --args <bin> <pocfile>
```

The decisive discriminator is **`rip` vs the fault address**:

- `rip` in valid module code, fault address is a data pointer → **data fault (DoS)**.
- `rip == fault address`, `rip` not in any module → **control transferred to a wild
  address and faulted trying to execute there** → **control-flow hijack**.

## 3. Crash-dump triage (real client)

Two independent, dependency-light tools over the on-disk minidumps:

- `dev/crash-triage/parse_minidump.py` — reads Breakpad binary structures directly
  (ExceptionStream → signal + fault address; AMD64 thread context → `rip`; ModuleList →
  which module `rip` is in; scans for `_GLIBCXX_ASSERTIONS` text). Classifies each dump
  as abort / near-null / canonical-wild / non-canonical, and whether `rip` is in a module.
- `minidump-stackwalk` (rust-minidump) — unwinds the crashing thread to
  `module+offset` frames, decodes the crash reason and the faulting instruction, and
  dumps the full register file.

Dump locations scanned: `~/dumps`, `~/.local/share/TelegramDesktop/tdata/dumps`,
`~/tgreport/raw` (100 dumps total).

**No symbol (`.sym`) files are on disk** for this build, so functions appear as
`telegram-desktop + 0xOFFSET`. Attribution to MicroTeX was therefore done by
**crash-signature and call-chain matching** against the gdb-characterized bugs (see
[`03_CRASH_DUMP_CORRELATION.md`](03_CRASH_DUMP_CORRELATION.md)), plus **live
reproduction**.

## 4. Live reproduction

The `\kern` payload was pasted into the running Telegram Desktop client, which crashed
and wrote fresh minidumps (08:19:06, 08:20:18). These were stackwalked immediately and
matched byte-for-byte in signature (crash reason, faulting instruction, caller chain) to
the earlier "mystery" wild-`rip` dumps — establishing that those were `\kern` too and
that the behavior is deterministic and software-induced (not thermal/hardware).

## 5. Honesty controls applied

- Verdicts on the shipping binary, not the sanitizer harness.
- "Not-in-any-module `rip`" cross-checked with `rip == fault` and with the stackwalker's
  own crash-reason decode before calling anything control-flow.
- The thermal/bit-flip alternative was raised explicitly and **falsified** by
  deterministic on-demand reproduction and by identical register state across dumps.
- RCE is claimed **only** as a *primitive/candidate*; no working payload was produced,
  and that limit is stated wherever the word "RCE" appears.
