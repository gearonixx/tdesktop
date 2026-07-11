# DOSSIER — `\kern` Control-Flow Hijack in MicroTeX `SpaceAtom::getFactor`

**Self-contained report for the Telegram Desktop / MicroTeX maintainers.**
Everything needed to understand, reproduce, and fix the bug is in this one file.

- **Component:** MicroTeX (bundled: `Telegram/ThirdParty/MicroTeX`)
- **Sink:** `tex::SpaceAtom::getFactor`, `src/atom/atom_space.h:45` (and the sibling
  `getSize`, line 49)
- **Trigger:** `\kern` (5 bytes), reachable from Instant View math
- **Class:** CWE-129 (improper validation of array index) → CWE-125 OOB read →
  **indirect call through out-of-bounds memory** (control-flow integrity violation)
- **Impact observed:** deterministic remote crash; **control transferred to a
  per-run-varying heap address** in the shipping client
- **Impact ceiling:** control-flow hijack **primitive**; remote code execution **not
  demonstrated**
- **Status:** **LIVE / unfixed** in current tree and shipping 6.9.x
- **Suggested severity:** **HIGH** (remote, near-zero-click, control-flow integrity),
  candidate **Critical** pending exploit-dev review

---

## 1. Attack surface / reachability

MicroTeX renders LaTeX for Telegram's Instant View articles. Attacker-controlled math in
`$…$` / `$$…$$` spans of an IV article reaches `tex::LaTeX::parse`
(`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`) and is rendered when
the victim **opens the article** — no further interaction. The render-time call chain
(from the crash dumps and gdb) is:

```
LaTeX::parse
  └ TeXRenderBuilder::build            (render.cpp)
      └ RowAtom::createBox             (atom_row.cpp:190)
          └ Dummy::createBox           (atom_row.cpp:32)
              └ SpaceAtom::createBox   (atom_space.cpp:19)   // \kern's atom
                  └ getFactor          (atom_space.h:45)     // <-- OOB indirect call
```

## 2. Root cause (exact source)

**Step 1 — `\kern` with no length yields `UnitType::none`.**
`src/core/macro_impl.cpp:12`
```cpp
macro(kern) {
  auto[unit, value] = tp.getLength();                 // no length after \kern
  return sptrOf<SpaceAtom>(unit, value, 0.f, 0.f);
}
```
`src/core/parser.cpp:693`
```cpp
pair<UnitType, float> TeXParser::getLength() {
  if (_pos == _len) return make_pair(UnitType::none, -1.f);   // <-- \kern at end of input
  ...
}
```
So the `SpaceAtom` is built with `_wUnit = UnitType::none`, `_width = -1`.

**Step 2 — `UnitType::none` is the integer −1.**
`src/utils/enums.h:99` (backing type `i8 = int8_t`, `src/utils/utils.h:15`)
```cpp
enum class UnitType : i8 {
  em, ex, pixel, point, pica, mu, cm, mm, in, sp, pt, dd, cc, x8,   // 0..13
  none = -1
};
```

**Step 3 — the unbounded index into the conversion array.**
`src/atom/atom_space.h:44`
```cpp
inline static float getFactor(UnitType unit, const Environment& env) {
  return _unitConversions[static_cast<i8>(unit)](env);      // <-- unit==none ⇒ index -1
}
inline static float getSize(UnitType unit, float size, const Environment& env) {
  return _unitConversions[static_cast<i8>(unit)](env) * size; // <-- same defect
}
```
`_unitConversions` is a fixed global array of **14** `std::function`s
(`src/atom/unit_conversion.cpp`), one per non-negative `UnitType` (indices 0..13). There
is **no bounds check**. `static_cast<i8>(none) == -1`, so `_unitConversions[-1]` reads the
`std::function`-sized object **32 bytes before** the array and **invokes it**.

`SpaceAtom::createBox` reaches the sink unconditionally for a non-blank space
(`src/atom/atom_space.cpp:17`):
```cpp
sptr<Box> SpaceAtom::createBox(Environment& env) {
  if (!_blankSpace) {
    float w = _width * getFactor(_wUnit, env);   // _wUnit == none  → OOB call
    ...
```

## 3. What `_unitConversions[-1](env)` actually does

`std::function<float(const Environment&)>` (libstdc++) is 32 bytes:

```
offset 0x00 : _Any_data      _M_functor   (16 bytes, union storage)
offset 0x10 : _Manager_type  _M_manager   (8 bytes, function pointer)
offset 0x18 : _Invoker_type  _M_invoker   (8 bytes, function pointer)
```

`operator()` compiles to:
```cpp
if (_M_empty()) __throw_bad_function_call();   // _M_empty() == (_M_manager == nullptr)
return _M_invoker(_M_functor, env);            // indirect CALL through _M_invoker
```

So the code:
1. treats the 32 bytes at `_unitConversions - 32` (a neighbouring global) as a
   `std::function`;
2. if the `_M_manager` slot there is non-null (passes the empty check),
3. **calls the pointer in the `_M_invoker` slot**, passing `&_M_functor` and `env`.

Whatever the linker placed immediately before `_unitConversions` becomes a fake callable.
Its `_M_invoker` bytes become the jump target.

## 4. GDB evidence — release build (`build_plain/plain`)

Reproduced directly (`ASAN_OPTIONS` irrelevant — non-ASan build). `\kern` → `SIGSEGV`,
20/20 deterministic. Backtrace and faulting instruction:

```
Program received signal SIGSEGV
#0  0x0000000000000000 in ?? ()                              <-- rip = 0x0  (executing at 0)
#1  std::function<float (tex::Environment const&)>::operator()
        (this=0x5555557b9cc0 <tex::PI>)                       <-- the fake std::function
        at bits/std_function.h:591
#2  tex::SpaceAtom::getFactor (unit=tex::UnitType::none)      atom_space.h:45
#3  tex::SpaceAtom::createBox                                 atom_space.cpp:19
#4  tex::TeXRenderBuilder::build                              render.cpp:195
#5  tex::LaTeX::parse                                         latex.cpp:186
=> 0x0: Cannot access memory at address 0x0
si_addr = 0x0
```

Read `#0` carefully: **`rip = 0x0`** — the process is *executing at address 0*, i.e. it
**called through** the `std::function` and jumped to the pointer it found. In this
particular binary the neighbouring global is `tex::PI` and its `_M_invoker`-slot bytes
resolve to `0x0`, so the jump target is null and it faults immediately. Frame `#1` names
the culprit: `std::function::operator()` on an object at `<tex::PI>` — an object that is
not actually a `std::function`.

> **Layout caveat (important):** whether the jump lands on `0x0` (this harness) or on a
> live heap address (the shipping client, §5) depends only on *which global sits at
> `_unitConversions[-1]`* and what its bytes decode to. The **defect is identical** in
> both; only the neighbour differs. This is why the sanitizer/harness view
> (“clean crash at 0x0”) understates the shipping-client behavior.

## 5. Real-client evidence — four Telegram Desktop crash dumps

telegram-desktop module range: **`0x555555554000 – 0x55555d561000`**. All four `\kern`
dumps put `rip` **above** that range, in the **heap**, and `rip == fault address`
(executing at a wild address):

| Dump | Time (2026-07-03) | `rip` (= fault, the call target) | Provenance |
|---|---|---|---|
| `a6c07f72-…` | 07:38:30 | `0x555570455100` | testing session |
| `83cf7b01-…` | 07:39:48 | `0x555573213180` | testing session |
| `71aaf1b3-…` | 08:19:06 | `0x55556ee671a0` | **live-reproduced `\kern`** |
| `e33d16a3-…` | 08:20:18 | `0x555574ca9e00` | **live-reproduced `\kern`** |

The target **varies every run** (heap ASLR) ⇒ it is read from memory, not a fixed code
path. Of **100** dumps on disk (2026-03-14 → today) these **4 are the only ones** with a
control-flow signature; all others crash with `rip` in valid code (safe DoS).

### 5.1 Register file (crashing thread, `e33d16a3`, live `\kern`)

```
rip = 0x0000555574ca9e00    <- CALL TARGET (_M_invoker); heap; varies per run
rdi = 0x00005555625ff060    <- &_M_functor: address of the fake std::function
                               (= _unitConversions[-1]); CONSTANT across all 4 dumps
rsi = 0x0000555564242f70    <- arg2 = Environment& env; heap; varies
rcx = 0x0000555563994280    <- constant across dumps
r8  = 0x0000000000000000    <- constant
r9  = 0x000055556236a7b8    <- constant
r10 = 0x000055556246ec90    <- constant
r14 = 0x00005555625ff080    <- constant (= rdi + 0x20)
rax = 0xffffffffffffffe0
rbp = 0x00007fffffffb350  rsp = 0x00007fffffffb308
```

**Register analysis.** The libstdc++ call is `_M_invoker(&_M_functor, env)`:
- `rdi = &_M_functor = 0x5555625ff060` — a **fixed global address** (constant in all four
  dumps). This is `_unitConversions[-1]`, the neighbouring global reinterpreted as a
  `std::function`.
- `rsi = env` — the live `Environment&` (heap, varies).
- `rip = _M_invoker` — the pointer read from `[0x5555625ff060 + 0x18]` — a **heap** value
  that **varies per run**.

Constant `rdi/rcx/r8/r9/r10` with varying `rsi/rip` across four independent crashes is
exactly the fingerprint of a deterministic indirect call `_M_invoker(&globalFunctor,
env)` — and rules out random memory corruption (which cannot reproduce identical register
state).

### 5.2 Call chain (identical in all four `\kern` dumps)

```
#0  0x5555_74ca_9e00              <- hijacked rip (wild heap target)
#1  telegram-desktop + 0x53d2677  <- SpaceAtom::createBox / getFactor (the indirect call site)
#2  telegram-desktop + 0xb77e11   <- Dummy::createBox
#3  telegram-desktop + 0xb79fe5   <- RowAtom::createBox
#4  telegram-desktop + 0xb883c3   <- TeXRenderBuilder::build
#5  telegram-desktop + 0x2f1f779
#6  telegram-desktop + 0x2f21670
#7  telegram-desktop + 0x2fcd672
```

The sub-chain `+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3` also appears in
same-session dumps that are *provably* MicroTeX (e.g. `9fb12b82`, whose `SIGABRT` carries
the assertion text `__n < this->size()` for `vector<shared_ptr<tex::Atom>>` = finding N4;
and `cc4c56bb`, a null-deref sharing the whole sub-chain). This anchors the wild-`rip`
crashes to the MicroTeX `SpaceAtom` render path — i.e. to `\kern`/`getFactor`. (No `.sym`
files are on disk, so frames are `module+offset`; attribution is by call-chain +
signature + live reproduction.)

### 5.3 Post-hijack faulting instruction

The stackwalker decodes the instruction at the hijacked `rip` as e.g.
`mov al, byte [0x10000555561fb26]` (a non-canonical read) → `SIGSEGV / SEGV_ACCERR`.
That is merely *what the heap bytes at the jump target happened to be*; it is a
consequence of the hijack, not the bug. Different heap contents ⇒ different executed
bytes ⇒ different outcome. This is why the finding is a control-flow-hijack **primitive**.

## 6. Determinism / the thermal hypothesis is falsified

The stackwalker prints a "crash address may be the result of a flipped bit" heuristic, and
the machine had been thermally throttling (~93 °C) shortly before the 07:38 dumps — so a
hardware bit-flip was considered and **rejected**:

- The bug reproduces **on demand** every time `\kern` is pasted (08:19, 08:20 captured
  live). Cosmic-ray/thermal bit-flips do not fire on a specific input.
- **Identical register state** across four separate crashes is impossible for random
  corruption.
- gdb on the release build reproduces the same indirect-call fault deterministically.

Conclusion: a deterministic **software** control-flow bug.

## 7. Exploitability (honest)

**What is proven:** a remotely-reachable, deterministic **indirect call through
out-of-bounds memory**, transferring control to a heap address, in the shipping client,
near-zero-click. This is a genuine control-flow-integrity violation and the strongest RCE
candidate found.

**What is not proven:** remote code execution. The jump currently lands on heap bytes that
decode to a faulting instruction → crash. Weaponization requires making the executed bytes
attacker-controlled, via one of:
- controlling the **`_M_invoker` slot** of the specific global at `_unitConversions[-1]`
  (a fixed global — needs analysis of *which* object it is and whether any of its bytes
  are influenced by input), or
- **heap grooming** from the same IV article so the (heap) jump target holds a controlled
  payload / fake-vtable, defeating the current non-canonical fault.

Both are non-trivial and were **not** attempted here. See
[`03_EXPLOITABILITY.md`](03_EXPLOITABILITY.md).

Mitigations already present that raise the bar: ASLR (target moves per run) and, for other
findings, `_GLIBCXX_ASSERTIONS`. Neither prevents *this* indirect call.

## 8. Proposed fix (bounds-check the sink)

Not applied in this draft. Guard both `getFactor` and `getSize`:

```cpp
// src/atom/atom_space.h
inline static float getFactor(UnitType unit, const Environment& env) {
  const auto i = static_cast<i8>(unit);
  if (i < 0 || i >= _unitsCount /* == count of _unitConversions */) return 0.f; // none/OOB → no space
  return _unitConversions[i](env);
}
inline static float getSize(UnitType unit, float size, const Environment& env) {
  const auto i = static_cast<i8>(unit);
  if (i < 0 || i >= _unitsCount) return 0.f;
  return _unitConversions[i](env) * size;
}
```

Defense in depth: have `macro(kern)` treat a `none` unit as zero-width/`pixel` so a missing
length can never produce `UnitType::none` downstream. Full patch and rationale:
[`04_PROPOSED_FIX.md`](04_PROPOSED_FIX.md).

> Note: `_unitConversions` has 14 entries but the code exposes only `_unitsCount` (the size
> of the *names* table). Confirm the bound used is the length of `_unitConversions`; if they
> can differ, introduce an explicit `_unitConversionsCount`.

## 9. Reproduce

PoC: [`poc/poc_kern.tex`](poc/poc_kern.tex) — the 5 bytes `\kern`.

```bash
# Release (true behavior), argv build:
cd /home/x/c/tdesktop/dev/microtex-fuzz
./build_plain/plain '\kern'                 # → SIGSEGV (rip transferred)

# Under gdb (see the indirect call):
gdb -q -batch -ex 'set debuginfod enabled off' -ex run -ex 'bt 5' \
    -ex 'info symbol $rip' -ex 'x/i $rip' --args ./build_plain/plain '\kern'

# In the real client: send/open an IV article whose math contains  $\kern$
#   → telegram-desktop writes a minidump to
#     ~/.local/share/TelegramDesktop/tdata/dumps  with rip==fault in the heap.

# ASan (root-cause corroboration only — see caveat below):
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
  ./build_afl2/afl_microtex_np ../new_draft/kern/poc/poc_kern.tex   # → global-buffer-overflow
```

ASan trace: [`asan_trace.txt`](asan_trace.txt) (captured from `build_afl2/afl_microtex_np`).
It reports a `global-buffer-overflow` READ of size 8 at `getFactor` (`atom_space.h:45`),
located **16 bytes before `tex::SpaceAtom::_unitConversions` / 8 bytes after `tex::PI`** —
i.e. it pins the `_unitConversions[-1]` read exactly. **Caveat:** ASan traps that OOB read
inside `std::_Function_base::_M_empty()` *before* the indirect call executes, so the trace
proves the **root cause** but **cannot** show the control transfer. The hijack itself
(`rip` → heap) is only visible in the release build (§4) and the real-client dumps (§5).

## 10. Summary line for a tracker

> Remotely-reachable OOB indirect call in MicroTeX `SpaceAtom::getFactor`
> (`_unitConversions[-1]` via `UnitType::none == -1`, triggered by `\kern`); transfers
> control to a heap address in shipping Telegram Desktop (confirmed by 4 crash dumps, live
> reproduced). Control-flow hijack primitive; RCE not demonstrated. Fix: bounds-check
> `getFactor`/`getSize`. Severity: HIGH (candidate Critical).
