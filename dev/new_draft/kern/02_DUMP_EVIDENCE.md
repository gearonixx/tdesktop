# Dump Evidence — raw GDB / stackwalk output

Developer-facing raw evidence for the `\kern` control-flow hijack. Reproduce with the
commands in [`01_METHODOLOGY.md`](../01_METHODOLOGY.md). No `.sym` files on disk → frames
are `telegram-desktop + offset`.

telegram-desktop module range: **`0x555555554000 – 0x55555d561000`**.

---

## A. Release-build gdb (`build_plain/plain '\kern'`)

```
Program received signal SIGSEGV, Segmentation fault.
#0  0x0000000000000000 in ?? ()
#1  0x0000555555631536 in std::function<float (tex::Environment const&)>::operator()
        (this=0x5555557b9cc0 <tex::PI>, __args=...) at bits/std_function.h:591
#2  0x000055555563140e in tex::SpaceAtom::getFactor (unit=tex::UnitType::none, env=...)
        at src/atom/atom_space.h:45
#3  0x000055555565be91 in tex::SpaceAtom::createBox (this=0x555555815460, env=...)
        at src/atom/atom_space.cpp:19
#4  0x00005555556b8ab9 in tex::TeXRenderBuilder::build (...) at src/render.cpp:195
#5  0x00005555556b86a4 in tex::TeXRenderBuilder::build (...) at src/render.cpp:168
#6  0x00005555555c259d in tex::LaTeX::parse (latex=L"\\kern", ...) at src/latex.cpp:186
#7  0x00005555555bfd67 in main (argc=2, argv=...) at plain_main.cpp:10

(gdb) info symbol $rip   ->  No symbol matches $rip.
(gdb) x/i $rip           ->  0x0: Cannot access memory at address 0x0
(gdb) p si_addr          ->  (void *) 0x0
```

Key: `#0 rip = 0x0` (executing at 0), reached **through**
`std::function::operator()` on `<tex::PI>` — a non-`std::function` global at
`_unitConversions[-1]`. Harness neighbour resolves to null; the shipping binary's does not
(section B).

---

## B. Real Telegram Desktop dumps

### B.1 `parse_minidump.py` classification (the four wild dumps)

```
a6c07f72-… | SIGSEGV @0x555570455100 (canonical-wild) | rip_in=<not-in-any-module> | RIP==FAULT
83cf7b01-… | SIGSEGV @0x555573213180 (canonical-wild) | rip_in=<not-in-any-module> | RIP==FAULT
71aaf1b3-… | SIGSEGV @0x55556ee671a0 (canonical-wild) | rip_in=<not-in-any-module> | RIP==FAULT   (live \kern)
e33d16a3-… | SIGSEGV @0x555574ca9e00 (canonical-wild) | rip_in=<not-in-any-module> | RIP==FAULT   (live \kern)
```

Whole-corpus scan: **100 dumps, exactly these 4** have `rip==fault` outside any module.
Earliest dump 2026-03-14, latest today; the signature first appears 2026-07-03 07:38.

### B.2 `minidump-stackwalk` — `e33d16a3` (live `\kern`)

```
Crash reason:  SIGSEGV / SEGV_ACCERR
Crash address: 0x0000555574ca9e00
Crashing instruction: `mov al, byte [0x10000555561fb26]`     (non-canonical read; post-hijack)

Thread 0 (crashed) - tid: 4105202
 0  0x555574ca9e00
     rax=0xffffffffffffffe0  rdx=0x0000555564242f70  rcx=0x0000555563994280  rbx=0x0000555575dc2440
     rsi=0x0000555564242f70  rdi=0x00005555625ff060  rbp=0x00007fffffffb350  rsp=0x00007fffffffb308
     r8 =0x0                 r9 =0x000055556236a7b8  r10=0x000055556246ec90  r11=0x0000555570542f30
     r12=0x00007fffffffb450  r13=0x0000555564242f70  r14=0x00005555625ff080  r15=0x00007fffffffb450
     rip=0x0000555574ca9e00     Found by: given as instruction pointer in context
 1  telegram-desktop + 0x53d2677     rip=0x000055555a926678   Found by: previous frame's frame pointer
 2  telegram-desktop + 0xb77e11      rip=0x00005555560cbe12
 3  telegram-desktop + 0xb79fe5      rip=0x00005555560cdfe6
 4  telegram-desktop + 0xb883c3      rip=0x00005555560dc3c4
 5  telegram-desktop + 0x2f1f779     rip=0x000055555847377a
 6  telegram-desktop + 0x2f21670     rip=0x0000555558475671
 7  telegram-desktop + 0x2fcd672     rip=0x0000555558521673
```

### B.3 `rip` variation across all four (target is read from memory → heap)

```
a6c07f72 (07:38): rip = 0x555570455100
83cf7b01 (07:39): rip = 0x555573213180
71aaf1b3 (08:19): rip = 0x55556ee671a0   (live)
e33d16a3 (08:20): rip = 0x555574ca9e00   (live)
```

All above the module (`…5d561000`), all heap, all different — while `rdi` (`&_M_functor`)
stays constant at `0x5555625ff060`. Signature of an indirect call, not fixed control flow.

### B.4 Constant registers across dumps (deterministic path proof)

| reg | a6c07f72 | 83cf7b01 | e33d16a3 | constant? |
|---|---|---|---|---|
| `rdi` (&functor) | `…625ff060` | `…625ff060` | `…625ff060` | **yes** |
| `rcx` | `…63994280` | `…63994280` | `…63994280` | **yes** |
| `r8`  | `0` | `0` | `0` | **yes** |
| `r9`  | `…6236a7b8` | `…6236a7b8` | `…6236a7b8` | **yes** |
| `r10` | `…6246ec90` | `…6246ec90` | `…6246ec90` | **yes** |
| `r14` | `…625ff080` | `…625ff080` | `…625ff080` | **yes** |
| `rip` (target) | `…70455100` | `…73213180` | `…74ca9e00` | no (heap, per-run) |
| `rsi`/`r13` (env) | varies | varies | varies | no (heap, per-run) |

Random corruption cannot produce identical `rdi/rcx/r8/r9/r10/r14` across four separate
crashes. This is a single deterministic code path.

---

## C. Same-session, provably-MicroTeX dumps (attribution anchors)

```
9fb12b82 (07:40) SIGABRT  assert "__n < this->size()"  [vector<std::shared_ptr<tex::Atom>>]
                 rip in libc (abort); callers include telegram-desktop+0x53d2677
                 == finding N4 (MatrixAtom::createBox ragged row) — PROVABLY MicroTeX
3f1b0d71 (07:39) SIGSEGV  fault=0x555574856000 (page-aligned), rip valid in telegram-desktop
                 == finding N2 (insertAtomIntoCol vector realloc OOB read)
cc4c56bb (07:43) SIGSEGV  fault=0x0, rip valid; caller chain
                 +0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3  (identical MicroTeX sub-chain)
                 == multirow / null-deref cluster
```

The shared caller sub-chain `+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3` between the
wild-`rip` dumps and these MicroTeX dumps is the link that ties the hijack to the MicroTeX
`SpaceAtom` render path. Combined with live reproduction, attribution to `\kern` is solid.
