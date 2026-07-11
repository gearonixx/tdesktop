# ALL — MicroTeX / Telegram Desktop Security Findings (single-file context dump)

> Self-contained consolidation of **every file in this folder** (`dev/new_draft/`):
> `README.md`, `00_EXECUTIVE_SUMMARY.md`, `01_METHODOLOGY.md`, `02_ALL_FINDINGS.md`,
> `03_CRASH_DUMP_CORRELATION.md`, `kern/*`, `other_findings/*`. Paste this into an LLM
> as the complete brief. **Status: DRAFT (internal), not yet reported upstream.**
> Date 2026-07-03. Target build: `/home/x/c/tdesktop`, Linux x86-64.

---

## 0. What this is / headline

Consolidated, honest write-up of every crash found across three AFL++ campaigns
(~100M+ executions) plus binary triage of **100** real Telegram Desktop crash dumps,
all **re-verified this session** two ways the earlier passes did not:

1. a **release (non-ASan) build driven under gdb** to get the *true* faulting
   instruction and `rip` (ASan redzones mask what the shipping binary actually does), and
2. **live reproduction in the real Telegram Desktop client**, correlated against the
   on-disk minidumps.

**Component:** bundled MicroTeX (`Telegram/ThirdParty/MicroTeX`).
**Entry point:** `tex::LaTeX::parse`, reached from Instant View (IV) markdown math
(`Telegram/SourceFiles/iv/markdown/iv_markdown_microtex.cpp:280`). Attacker-controlled
LaTeX in `$…$` / `$$…$$` math spans of an IV article is parsed + rendered when the victim
**opens the article** — **near-zero-click**. telegram-desktop module range this session:
`0x555555554000 – 0x55555d561000`.

| | |
|---|---|
| **1 control-flow hijack primitive** | **`\kern`** → `SpaceAtom::getFactor` indirect call through out-of-bounds memory. **Confirmed live in the shipping client** (control transfers to a per-run-varying **heap** address). Strongest RCE candidate; RCE **not** demonstrated. **UNFIXED / live in 6.9.x.** |
| **~6 DoS bugs** | multirow null-deref, N2 `insertAtomIntoCol` OOB read, N4 ragged `createBox` (hardened → abort), null-deref cluster (~4 confirmed sites), N1 `wchar_t`→ctype, plus a large hang bucket. All crash-only. |
| **Non-bugs** | `\substack`/`dynamic_pointer_cast @0x0d` (libFuzzer RTTI artifact), the accent/IndexedArray bug (already fixed), `smallmatrix` UAF (ASan-only tripwire), and fabricated "non-crashes." |
| **RCE demonstrated?** | **No.** One genuine control-flow-hijack *primitive* (`\kern`); no working write/target-control PoC produced. |

**One-line verdict:** a remotely-reachable **control-flow hijack primitive** (`\kern`) is
confirmed live in shipping Telegram Desktop; it currently *crashes* rather than executes
attacker code, so it is a **credible RCE candidate, not a demonstrated RCE**. Everything
else is denial-of-service.

**Corrected record:** an earlier pass called the whole campaign "all DoS, nothing
dangerous." That was **wrong for `\kern`** — the error came from trusting an ASan/harness
repro where the `_unitConversions[-1]` slot happened to resolve to `0x0` (clean crash),
instead of verifying against the real client, where the same slot resolves to a **heap
pointer** and the process **transfers control into the heap**. The correction was forced by
driving the release build under gdb and by the user's live reproduction.

---

## 1. THE FINDING THAT MATTERS — `\kern` control-flow hijack

- **Sink:** `tex::SpaceAtom::getFactor`, `src/atom/atom_space.h:45` (and sibling `getSize`, line 49).
- **Trigger:** `\kern` (5 bytes, no length argument), reachable from IV math.
- **Class:** CWE-129 (improper array-index validation) → CWE-125 OOB read →
  **indirect call through out-of-bounds memory** (control-flow-integrity violation).
- **Impact observed:** deterministic remote crash; **control transferred to a
  per-run-varying heap address** in the shipping client. `rip == fault`, outside any module.
- **Impact ceiling:** control-flow hijack **primitive**; RCE **not demonstrated**.
- **Status:** **LIVE / unfixed** in current tree and shipping 6.9.x.
- **Suggested severity:** **HIGH** (remote, near-zero-click, CFI violation), candidate
  **Critical** pending exploit-dev review.

### 1.1 Reachability (render-time call chain, from dumps + gdb)
```
LaTeX::parse
 └ TeXRenderBuilder::build          (render.cpp)
   └ RowAtom::createBox             (atom_row.cpp:190)
     └ Dummy::createBox             (atom_row.cpp:32)
       └ SpaceAtom::createBox       (atom_space.cpp:19)   // \kern's atom
         └ getFactor                (atom_space.h:45)     // <-- OOB indirect call
```

### 1.2 Root cause (exact source, 3 steps)

**Step 1 — `\kern` with no length yields `UnitType::none`.**
```cpp
// src/core/macro_impl.cpp:12
macro(kern) {
  auto[unit, value] = tp.getLength();          // no length after \kern
  return sptrOf<SpaceAtom>(unit, value, 0.f, 0.f);
}
// src/core/parser.cpp:693
pair<UnitType, float> TeXParser::getLength() {
  if (_pos == _len) return make_pair(UnitType::none, -1.f);   // \kern at end of input
  ...
}
```
So the `SpaceAtom` is built with `_wUnit = UnitType::none`, `_width = -1`.

**Step 2 — `UnitType::none` is the integer −1.**
```cpp
// src/utils/enums.h  (backing type i8 = int8_t, src/utils/utils.h:15)
enum class UnitType : i8 {
  em, ex, pixel, point, pica, mu, cm, mm, in, sp, pt, dd, cc, x8,  // 0..13
  none = -1
};
```

**Step 3 — unbounded index into the conversion array.**
```cpp
// src/atom/atom_space.h:44
inline static float getFactor(UnitType unit, const Environment& env) {
  return _unitConversions[static_cast<i8>(unit)](env);        // unit==none ⇒ index -1
}
inline static float getSize(UnitType unit, float size, const Environment& env) {
  return _unitConversions[static_cast<i8>(unit)](env) * size; // same defect
}
```
`_unitConversions` is a fixed global array of **14** `std::function`s
(`src/atom/unit_conversion.cpp`, indices 0..13, one per non-negative `UnitType`). **No
bounds check.** `static_cast<i8>(none) == -1` → `_unitConversions[-1]` reads the
`std::function`-sized object **32 bytes before** the array and **invokes it**.
`SpaceAtom::createBox` reaches the sink unconditionally for a non-blank space
(`atom_space.cpp:17`: `float w = _width * getFactor(_wUnit, env);`).

### 1.3 What `_unitConversions[-1](env)` actually does (libstdc++ std::function ABI)
`std::function<float(const Environment&)>` is 32 bytes:
```
0x00 : _Any_data      _M_functor   (16 bytes, union storage)
0x10 : _Manager_type  _M_manager   (8 bytes fn ptr; null ⇒ empty)
0x18 : _Invoker_type  _M_invoker   (8 bytes fn ptr; the actual call)
```
`operator()` compiles to:
```cpp
if (_M_empty()) __throw_bad_function_call();   // _M_empty() == (_M_manager == nullptr)
return _M_invoker(_M_functor, env);            // indirect CALL through _M_invoker
```
So on the neighbouring global `G = _unitConversions - 32`: read `G._M_manager` (if null,
throw — not observed); else **call `G._M_invoker`** with `arg0 = &G._M_functor (== &G)`,
`arg1 = env`. Whatever the linker placed before `_unitConversions` becomes a fake callable;
its `_M_invoker` bytes become the jump target.

### 1.4 gdb evidence — release build (`dev/microtex-fuzz/build_plain/plain '\kern'`)
`\kern` → SIGSEGV, 20/20 deterministic (ASan irrelevant — non-ASan build):
```
Program received signal SIGSEGV
#0  0x0000000000000000 in ?? ()                              <-- rip = 0x0 (executing at 0)
#1  std::function<float (tex::Environment const&)>::operator()
        (this=0x5555557b9cc0 <tex::PI>) at bits/std_function.h:591   <-- fake std::function
#2  tex::SpaceAtom::getFactor (unit=tex::UnitType::none)      atom_space.h:45
#3  tex::SpaceAtom::createBox                                 atom_space.cpp:19
#4  tex::TeXRenderBuilder::build                              render.cpp:195
#5  tex::LaTeX::parse (latex=L"\\kern")                       latex.cpp:186
=> 0x0: Cannot access memory at address 0x0 ; si_addr = 0x0
```
`#0 rip = 0x0`: the process **called through** the std::function and jumped to the pointer
it found. In *this* binary the neighbour global is `tex::PI` and its `_M_invoker`-slot bytes
resolve to `0x0` → null jump → immediate fault.

> **Layout caveat (critical):** whether the jump lands on `0x0` (this harness) or a live
> **heap** address (the shipping client) depends only on *which global sits at
> `_unitConversions[-1]`* and what its bytes decode to. **The defect is identical;** only
> the neighbour differs. The harness's tidy `0x0` crash *understates* the shipping behavior
> — always read this bug from the real-client dumps.

### 1.5 Real-client evidence — 4 Telegram Desktop crash dumps
All four put `rip` **above** the module range, in the **heap**, with `rip == fault` (executing at a wild address):

| Dump | Time (2026-07-03) | `rip` (= fault = call target) | Provenance |
|---|---|---|---|
| `a6c07f72-…` | 07:38:30 | `0x555570455100` | testing session |
| `83cf7b01-…` | 07:39:48 | `0x555573213180` | testing session |
| `71aaf1b3-…` | 08:19:06 | `0x55556ee671a0` | **live-reproduced `\kern`** |
| `e33d16a3-…` | 08:20:18 | `0x555574ca9e00` | **live-reproduced `\kern`** |

Target **varies every run** (heap ASLR) ⇒ read from memory, not a fixed code path. Of
**100** dumps on disk (2026-03-14 → today) these **4 are the only ones** with a
control-flow signature; all others crash with `rip` in valid code (safe DoS). Signature
first appears 2026-07-03 07:38.

**Register file (crashing thread, `e33d16a3`, live `\kern`):**
```
rip = 0x0000555574ca9e00    <- CALL TARGET (_M_invoker); heap; varies per run
rdi = 0x00005555625ff060    <- &_M_functor = _unitConversions[-1]; CONSTANT across all 4 dumps (fixed global)
rsi = 0x0000555564242f70    <- arg2 = Environment& env; heap; varies
rcx = 0x0000555563994280    <- constant    r8  = 0x0            <- constant
r9  = 0x000055556236a7b8    <- constant    r10 = 0x000055556246ec90 <- constant
r14 = 0x00005555625ff080    <- constant (= rdi + 0x20)          rax = 0xffffffffffffffe0
rbp = 0x00007fffffffb350    rsp = 0x00007fffffffb308
```
Libstdc++ call is `_M_invoker(&_M_functor, env)`: `rdi` = fixed global (`&G`), `rsi` = live
`Environment&`, `rip` = pointer read from `[G + 0x18]` (heap, varies). **Constant
`rdi/rcx/r8/r9/r10/r14` with varying `rsi/rip` across four independent crashes** is exactly
the fingerprint of a deterministic indirect call — and **rules out random corruption**
(which cannot reproduce identical register state).

**Call chain (identical in all four `\kern` dumps; no `.sym` on disk → module+offset):**
```
#0  0x5555_74ca_9e00              <- hijacked rip (wild heap target)
#1  telegram-desktop + 0x53d2677  <- SpaceAtom::createBox / getFactor (indirect call site)
#2  telegram-desktop + 0xb77e11   <- Dummy::createBox
#3  telegram-desktop + 0xb79fe5   <- RowAtom::createBox
#4  telegram-desktop + 0xb883c3   <- TeXRenderBuilder::build
#5..#7  +0x2f1f779 / +0x2f21670 / +0x2fcd672
```
Post-hijack faulting instruction (stackwalk): e.g. `mov al, byte [0x10000555561fb26]`
(non-canonical read) → `SIGSEGV / SEGV_ACCERR` — merely *what the heap bytes at the target
decoded to*, a consequence of the hijack, not the bug. Different heap ⇒ different outcome →
this is a **primitive** whose impact depends on heap state.

### 1.6 Attribution anchor (ties wild-`rip` to MicroTeX)
The sub-chain `+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3` also appears in
same-session dumps that are *provably* MicroTeX: `9fb12b82` (07:40, `SIGABRT` carrying
assertion text `__n < this->size()` for `vector<shared_ptr<tex::Atom>>` = finding N4);
`cc4c56bb` (07:43, null-deref sharing the whole sub-chain). Plus **live reproduction**
(08:19/08:20 dumps produced on demand by pasting `\kern`, matching the earlier "mystery"
dumps byte-for-byte in reason/instruction/registers/caller chain).

### 1.7 Determinism / thermal hypothesis FALSIFIED
The stackwalker prints a "crash address may be the result of a flipped bit" heuristic, and
the machine had thermally throttled (~93 °C) shortly before the 07:38 dumps — so a hardware
bit-flip was **considered and rejected**: (a) reproduces on demand every time `\kern` is
pasted; (b) identical register state across four crashes is impossible for random
corruption; (c) gdb on the release build reproduces the same indirect-call fault
deterministically. Conclusion: a deterministic **software** control-flow bug.

### 1.8 Exploitability (honest — calibration, not marketing)
Primitive = **indirect call through attacker-*reachable* OOB memory**. `rip` becomes a
value read from `_unitConversions[-1]._M_invoker`. Scored against RCE requirements:

| Requirement for RCE | Status | Notes |
|---|---|---|
| Control-flow transfer to non-fixed target | ✅ yes | `rip` = OOB value; lands in heap; varies per run |
| Target in attacker-influenceable region | ⚠️ partial | it's the heap (sprayable), but specific target not shown populatable |
| Target **bytes** attacker-controlled | ❌ not shown | currently incidental heap bytes → faulting instruction → crash |
| Value control of invoker slot | ❌ not shown | `_M_invoker` from a **fixed global**; need to ID it + whether input touches it |
| Info leak (defeat ASLR) | ❌ none | OOB value consumed by the call, not returned |
| Reliability | ⚠️ deterministic **crash**, not deterministic **jump-to-controlled** | 100% crash; controlled execution not achieved |

Two top items unmet → **credible control-flow-hijack primitive, RCE not demonstrated.**
Two realistic (unattempted) weaponization paths: (1) **control the invoker slot** — ID the
fixed global at `_unitConversions[-1]` (was `tex::PI` in the harness) and find an
input-influenced pointer field; (2) **groom the heap target** via same-article allocations
so the jump range holds a controlled gadget (heap-spray + CFI-bypass; hard under ASLR).
Mitigations present that raise the bar but don't stop this call: ASLR, `_GLIBCXX_ASSERTIONS`.

### 1.9 Proposed fix (NOT applied — no source modified in this draft)
Bounds-check both sinks in `src/atom/atom_space.h`:
```cpp
inline static float getFactor(UnitType unit, const Environment& env) {
  const auto i = static_cast<i8>(unit);
  if (i < 0 || i >= _unitConversionsCount) return 0.f;   // none/OOB → no space
  return _unitConversions[i](env);
}
inline static float getSize(UnitType unit, float size, const Environment& env) {
  const auto i = static_cast<i8>(unit);
  if (i < 0 || i >= _unitConversionsCount) return 0.f;
  return _unitConversions[i](env) * size;
}
```
**Bound caveat:** `_unitConversions` has **14** entries; the existing `_unitsCount` is the
size of the *names* table `_units[]` (**16** aliases) — a **different** number. Do NOT reuse
`_unitsCount`; add an explicit count:
```cpp
// unit_conversion.cpp
const i32 SpaceAtom::_unitConversionsCount = sizeof(_unitConversions)/sizeof(_unitConversions[0]); // ==14
```
Defense in depth — reject `none` at the macro (`macro_impl.cpp:12`):
```cpp
macro(kern) {
  auto[unit, value] = tp.getLength();
  if (unit == UnitType::none) { unit = UnitType::pixel; value = 0.f; } // no length -> zero-width
  return sptrOf<SpaceAtom>(unit, value, 0.f, 0.f);
}
```
Returning `0.f` for an invalid unit is safest/most-local (matches `getUnit` fallback,
`atom_space.cpp:13`). **Test plan:** (1) `./build_plain/plain '\kern'` must exit 0 (was
SIGSEGV); `$\kern$` in real client writes no minidump. (2) valid spaces unchanged
(`\kern 3pt`, `\hspace{1cm}`, `\,`/`\;`/`\quad`, `\rule{1pt}{1pt}`). (3) exercise `getSize`
with malformed unit. (4) re-run the AFL corpus that produced N3 — `getFactor` OOB signature
gone.

### 1.10 Tracker summary line
> Remotely-reachable OOB indirect call in MicroTeX `SpaceAtom::getFactor`
> (`_unitConversions[-1]` via `UnitType::none == -1`, triggered by `\kern`); transfers
> control to a heap address in shipping Telegram Desktop (4 crash dumps, live reproduced).
> Control-flow hijack primitive; RCE not demonstrated. Fix: bounds-check
> `getFactor`/`getSize`. Severity HIGH (candidate Critical).

---

## 2. Everything else — DoS findings

### #2 multirow — `\multirow` heap OOB in `MatrixAtom::recalculateLine`
- **Trigger:** `\begin{array}{c}\multirow{9}{*}{x}\end{array}` (span > rows). **Site:** `atom_matrix.cpp:315`.
- **Root cause:** adjustment loop uses `mr = m->_i + m->_n` **not clamped to `rows`**; the
  counting loop just above *does* guard `j < rows`, the adjustment loop does not. `boxarr =
  new sptr<Box>*[rows]`, `height/depth = new float[rows]` (`:441/:439`). Span past last row → `j >= rows`.
- **gdb (release):** `boxarr[j]` (OOB) reads back **NULL** → `boxarr[j][0]->_type`
  null-derefs at `0x0`, `rip` in valid code. The `height[j] += ex` **write two lines down is
  never reached** — the gating read faults first. 20/20 deterministic (spans 9/50/200/900 all
  fault at `:315`). **So the earlier "controlled heap write / CWE-787" framing is wrong in
  practice — it is a null-deref DoS.** (ASan calls it heap-OOB READ size 8 at `:315`.)
- **Fix:** `const int mr = std::min(m->_i + m->_n, rows);` (present in local tree, not
  upstreamed; live in shipping). `kMaxArrayRowSpan=1000` bounds `|n|` but doesn't tie it to `rows`.
- Two-line variant (`…\multirow{5}…\n…\multirow{20}…`) = **identical** crash (same `rip`, `:315`, fault `0x0`); not distinct.

### #3 N2 — OOB in `ArrayFormula::insertAtomIntoCol`
- **Trigger:** `\begin{cases}\\&\end{cases}` (16-byte min: `\begin{cases}\\&`). **Site:** `formula.cpp:214`.
- **Root cause:** `_array[j].insert(it + col, atom)` applies the current `col` to **every
  prior row**; a shorter earlier row makes `it + col` an iterator **past `end()`**. `\\`
  (addRow) leaves a short row while `_row` still counts it; `&` (addCol) inserts at `col` beyond its length.
- **gdb (release):** capacity exceeded → `_M_realloc_insert` relocates `[position, end())`;
  `position` past `end()` → `__relocate_a` walks off the old buffer → read across a **page
  boundary** (`0x…c000`) → SIGSEGV, `rip` in valid libstdc++ code → **data fault, DoS**.
  Relocation writes an internal `Atom*` (not attacker data); no controlled write demonstrated.
- **Fix:** clamp per row: `pos = std::min<size_t>(col<0?0:col, row.size()); row.insert(row.begin()+pos, atom);`
  Same *ragged-row* family as N4 but a **parse-time** sink; each needs its own guard. **Live.**

### #4 N4 — ragged-row OOB in `MatrixAtom::createBox` (hardened → safe abort)
- **Trigger:** `\begin{tabular}0\\&\hline` (25-byte min). **Site:** `atom_matrix.cpp:595`
  (ASan reported `:603` on a differently-built tree; same hline-adjacency check).
- **Root cause:** `if (i>=1 && dynamic_cast<HlineAtom*>(_matrix->_array[i-1][j].get()))` —
  `j` driven by the wider current row, `_array[i-1][j]` indexes past the end of the shorter previous row.
- **gdb (release):** access is via `std::vector::operator[]`, so `_GLIBCXX_ASSERTIONS`
  bounds-checks it → `stl_vector.h:1253 Assertion '__n < this->size()' failed` → clean
  **`abort()` before any wild read** (SIGABRT, `rip` in libc). Safest outcome. Contrast
  multirow (raw `new[]`, no such protection).
- **Fix:** `auto& prev = _matrix->_array[i-1]; if (i>=1 && j < (int)prev.size() && dynamic_cast<HlineAtom*>(prev[j].get())) {…}`
- **Note:** the one finding *proven* to reach the real client as a MicroTeX crash — dump
  `9fb12b82` (07:40) carries the exact assertion text = attribution anchor for the session. **Live.**

### #5 null-deref cluster — CWE-476 DoS (likely one root cause)
Many `createBox()`/accessors unconditionally deref a child/base atom; the parser can produce
a null `sptr<Atom>` from malformed/empty macro args that propagates unchecked. `rip` stays
in valid code, fault at fixed low addr (`0x0`/`0x8`). Confirmed sites (gdb, release):

| Site | Function | Min trigger | Fault |
|---|---|---|---|
| `atom_impl.h:76` | `CedillaAtom::createBox` | `\c{}{2}` | `0x0` |
| `atom_matrix.cpp:691` | `MulticolumnAtom::createBox` | `\begin{array}{}\multicolumn{2}{` | `0x0` (highest-count) |
| `atom_basic.h:528` | `UnderOverAtom::leftType` (via `Dummy::leftType`) | `poc_underover_*.bin` | `0x0` |
| `atom_basic.h:115` | `ScaleAtom` ctor (via `MonoScaleAtom`, `\tiny`-class) | `frac{\tiny ` | `0x8` |

**Fix:** single choke-point — substitute `EmptyAtom` for a null macro-arg result (MicroTeX
already does this in the partial-parse path, `formula.cpp:50`); would likely close most of
the cluster. ~15 more sites bucketed but not individually root-caused; confirm distinct
count before filing. **Live.**

### #6 N1 — raw `wchar_t` into `<cctype>` (DoS / benign-in-release)
- **Trigger:** astral (non-BMP) codepoint in a command/env name (e.g. after `\begin{env}…`),
  99-byte PoC. **Site:** `isalpha` ← `TeXParser::isValidCharInCmd` `parser.h:354` (also `isValidName`).
- **Root cause:** glibc `isalpha`/`isalnum` index `__ctype_b_loc()[c]`, defined only for
  `c ∈ [-1,255]`; MicroTeX passes a raw `wchar_t` (up to U+10FFFF) → far-OOB table read.
- **Behavior:** under **ASan** → wild OOB read → SIGSEGV (how it was found). Under the
  **release** build the PoC **exits 0** — the index lands on mapped memory, garbage
  classification merely mis-parses, no crash. Lowest priority. **Fix:** mask to `unsigned
  char` / guard `c < 128` before `<cctype>`, or use a Unicode-aware predicate.

### #7 hangs — resource-exhaustion DoS
Largest crash-corpus bucket (~1,438 `TIMEOUT_or_SILENT`): deep parser recursion /
pathological loops (CPU/stack exhaustion, not memory safety). Un-minimized; `rip`/fault n/a.
**Fix:** input-size / nesting-depth caps (some exist: `kMaxArrayCells`, `kMaxArrayRowSpan`).

---

## 3. NOT bugs / already fixed (kept on record so nobody re-files them)

- **`dynamic_pointer_cast<MiddleAtom>` SEGV `@0x0d` (a.k.a. F-OPEN-1) — NOT A BUG.**
  Triggers `\substack{a\\b}`, `\color{red}{x}`, `\left(x\middle|y\right)`. **libFuzzer
  harness artifact:** exits 0 under argv (ASan) and AFL non-persistent (ASan); only faults
  when `-fsanitize=fuzzer` links a second, divergent copy of C++ RTTI, so `dynamic_cast`
  over the multiply-inherited `Atom` hierarchy walks a foreign `type_info` → SEGV on valid
  objects. The same AFL build that exits 0 here still catches real bugs (`\kern` crashes),
  proving it's the fuzzer runtime, not the input. **Zero product impact** (Telegram has no
  libFuzzer runtime). `formula.cpp:110` is indeed `dynamic_pointer_cast<MiddleAtom>` but not defective.
- **`smallmatrix` UAF — ASan tripwire only.** `\begin{smallmatrix}\end{smallmatrix}`:
  `Environment::operator=` self-assigns via `env = *(e.copy())` (`atom_matrix.cpp:448`),
  freeing+reading the same `Environment` in one memberwise copy. ASan flags heap-UAF; the
  **release build exits 0** (freed bytes intact for the rest of the copy, no realloc window).
  Worth a tidy fix (`Environment tmp = *(e.copy()); env = tmp;`) but **not weaponizable**.
- **accent / `IndexedArray` OOB (F1) — already FIXED.** `AccentedAtom::createBox` →
  `FontInfo::getNextLarger` off-by-one OOB read on any accented Latin-1 char (`$é$`). Fixed
  in tree; PoC `findings/bug1_accent_oob/poc_min_1byte.bin` exits 0.
- **Inputs that simply do not crash.** `\begin{array}{cc}&\\\\\end{array}` and
  `\begin{matrix}&\\\\\end{matrix}` — exit 0, any backslash count 1–6. A previously-circulated
  "null-deref @ `formula.cpp:25`" label was **fabricated** — line 25 is a logging block in
  `Formula::_init_()`, not a dereference. Do not file.

---

## 4. Methodology (reproducible; paths literal)

**Builds** (MicroTeX src: `Telegram/ThirdParty/MicroTeX/src`):

| Build | Path | Sanitizer | Input | Purpose |
|---|---|---|---|---|
| ASan argv | `dev/microtex-fuzz/build_a/plain` | ASan | `argv[1]` | fast triage, ASan reports |
| **Release argv** | `dev/microtex-fuzz/build_plain/plain` | none (`-DNDEBUG`, `_GLIBCXX_ASSERTIONS`) | `argv[1]` | **true faulting behavior** |
| Release file | built this session → `scratchpad/rel_file` | none | file (exact bytes incl. NUL) | PoCs with NUL/high bytes |
| AFL non-persistent | `dev/microtex-fuzz/build_afl2/afl_microtex_np` | ASan+UBSan (vptr off) | file | crash-corpus replay |

**Why release matters:** ASan redzones/allocator intercept make an OOB *read* fault inside
ASan *before* downstream code runs — masking what the shipping binary does. Every
shape verdict (data-fault vs control-transfer) was taken from the **release** build, then
corroborated against real dumps. Sanity control: release harness must reproduce a known
crash (`\kern`→SIGSEGV) and ASan must flag a known bug on every sweep.

**gdb procedure (core of the report):**
```
gdb -q -batch -ex 'set debuginfod enabled off' -ex run \
  -ex 'bt' -ex 'info symbol $rip' -ex 'x/i $rip' \
  -ex 'print $_siginfo._sifields._sigfault.si_addr' --args <bin> <pocfile>
```
Decisive discriminator = **`rip` vs fault address**: `rip` in valid module code + fault a
data pointer → **data fault (DoS)**; `rip == fault` and `rip` not in any module → **control
transferred to a wild address → control-flow hijack**.

**Crash-dump triage (real client), two independent tools over on-disk minidumps
(`~/dumps`, `~/.local/share/TelegramDesktop/tdata/dumps`, `~/tgreport/raw` = 100 dumps):**
- `dev/crash-triage/parse_minidump.py` — reads Breakpad structures directly (ExceptionStream
  → signal+fault; AMD64 thread context → `rip`; ModuleList → module; scans for
  `_GLIBCXX_ASSERTIONS` text); classifies abort / near-null / canonical-wild / non-canonical,
  and whether `rip` is in a module.
- `minidump-stackwalk` (rust-minidump) — unwinds to `module+offset`, decodes crash reason +
  faulting instruction, dumps the register file.
- **No `.sym` files on disk** → functions appear as `telegram-desktop + 0xOFFSET`;
  attribution to MicroTeX is by **crash-signature + call-chain matching** + **live repro**.

**Live reproduction:** `\kern` pasted into the running client → fresh minidumps (08:19:06,
08:20:18), stackwalked immediately, matched byte-for-byte to the earlier "mystery" wild-`rip`
dumps → those were `\kern` too; deterministic + software-induced (not thermal/hardware).

**Honesty controls:** verdicts on the shipping binary not the harness; "not-in-any-module
`rip`" cross-checked with `rip == fault` and the stackwalker's own reason decode; the
thermal/bit-flip alternative raised explicitly and **falsified**; "RCE" claimed **only** as
a primitive/candidate, with the limit stated wherever the word appears.

---

## 5. Real-client crash-dump correlation (the 100-dump picture)

- Across all **100** dumps (2026-03-14 → 2026-07-03), **exactly 4** have `rip == fault` with
  `rip` outside any module — all 2026-07-03, in two clusters (see §1.5). The other **96**
  (incl. the 89 from the prior triage) crash with `rip` in valid code = safe DoS.
- **Constant-register proof** (deterministic path, not random corruption): across
  `a6c07f72`/`83cf7b01`/`e33d16a3`, `rdi`=`…625ff060`, `rcx`=`…63994280`, `r8`=`0`,
  `r9`=`…6236a7b8`, `r10`=`…6246ec90`, `r14`=`…625ff080` are all **constant**; `rip` (target)
  and `rsi`/`r13` (env) vary (heap, per-run).
- **Same-session provably-MicroTeX dumps** anchor attribution: `9fb12b82` (07:40, N4 SIGABRT
  with `__n < size()` assertion text), `3f1b0d71` (07:39, N2 page-boundary SIGSEGV, `rip`
  valid), `cc4c56bb` (07:43, multirow/null-cluster SIGSEGV@0x0, `rip` valid) — the last shares
  the identical MicroTeX sub-chain `+0x53d2677 → +0xb77e11 → +0xb79fe5 → +0xb883c3`.
- **Unrelated:** the pre-existing 89-dump triage (`dev/crash-triage/`) found **0** MicroTeX
  and **0** control-flow crashes — a separate `std::vector<Dialogs::Key>::operator[]`
  bounds-`abort()` + null-deref cluster in the chat-list code, all safe DoS. The `\kern`
  signature is genuinely new (first seen 2026-07-03).

---

## 6. Severity ordering (by gdb-observed shape, worst first)

1. **`\kern`** — control transferred to a wild heap address (`rip == fault`). *Only* finding
   that violates control-flow integrity. → **HIGH**, candidate Critical.
2. **N2** — OOB read during vector reallocation (raw internal path, not assertion-guarded).
3. **multirow** — null-deref on raw `sptr<Box>**`/`float[]`; write unreachable (gated).
4. **null-deref cluster** — null-deref in app code, fixed low fault address.
5. **N4** — same family as multirow but on `std::vector`, so `_GLIBCXX_ASSERTIONS` → clean abort (safest).
6. **N1 / hangs** — value-dependent read / resource exhaustion.

Ranking shorthand: `\kern` ≫ multirow ≥ N2 > cluster > N4; N1/hangs lowest.

---

## 7. Reproduce cheatsheet + file index

```bash
cd /home/x/c/tdesktop/dev/microtex-fuzz
./build_plain/plain '\kern'                    # release → SIGSEGV (rip transferred)
gdb -q -batch -ex 'set debuginfod enabled off' -ex run -ex 'bt 5' \
    -ex 'info symbol $rip' -ex 'x/i $rip' --args ./build_plain/plain '\kern'
# real client: open an IV article whose math contains  $\kern$  → minidump in
#   ~/.local/share/TelegramDesktop/tdata/dumps  with rip==fault in the heap.
```
Other triggers (release argv build): `\begin{array}{c}\multirow{9}{*}{x}\end{array}` (#2),
`\begin{cases}\\&` (N2), `\begin{tabular}0\\&\hline` (N4), `\c{}{2}` / `\begin{array}{}\multicolumn{2}{` (cluster).

**Source files in this folder** (this ALL.md consolidates all of them):
- `README.md` — nav + one-paragraph verdict.
- `00_EXECUTIVE_SUMMARY.md` — one-page version.
- `01_METHODOLOGY.md` — builds, gdb procedure, dump-triage tooling, honesty controls.
- `02_ALL_FINDINGS.md` — master findings table.
- `03_CRASH_DUMP_CORRELATION.md` — 100 dumps; the 4 `\kern` control-flow dumps; live repro.
- `kern/` — the `\kern` hijack in full depth: `00_DOSSIER.md` (self-contained dev report),
  `01_MECHANISM.md` (byte-level std::function ABI), `02_DUMP_EVIDENCE.md` (raw gdb/stackwalk),
  `03_EXPLOITABILITY.md` (honest RCE analysis), `04_PROPOSED_FIX.md` (patch + test plan),
  `poc/poc_kern.tex` (the 5 bytes `\kern`).
- `other_findings/` — one file per DoS finding + `NOT_bugs.md`.
