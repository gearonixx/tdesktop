# Finding #2 — `\multirow` heap OOB in `MatrixAtom::recalculateLine` (DoS)

**Trigger:** `\begin{array}{c}\multirow{9}{*}{x}\end{array}` (span > rows)
**Site:** `src/atom/atom_matrix.cpp:315` (adjustment loop in `recalculateLine`)
**Class:** heap OOB read → null-deref crash. A latent OOB *write* exists in source but is
**unreachable** (gated). **DoS.**
**Fix status:** fixed in local tree (`min(_i+_n, rows)`), **not upstreamed**; live in
shipping.

## Root cause

`\multirow{n}` sets a row span `n`. The adjustment loop spreads extra height/depth across
spanned rows:

```cpp
// pre-fix (shipping):
const int mr = m->_i + m->_n;              // NOT clamped to rows
for (int j = m->_i; j < mr; j++) {
  if (boxarr[j][0]->_type != AtomType::hline) {   // :315  boxarr[j] OOB when j >= rows
    height[j] += ex;                              // OOB write past new float[rows]
    depth[j]  += ex;
  }
}
```

`boxarr` = `new sptr<Box>*[rows]`, `height`/`depth` = `new float[rows]`
(`atom_matrix.cpp:441/439`). A span past the last row makes `j >= rows`, indexing all three
raw arrays out of bounds. The *counting* loop just above **does** guard `j < rows`; the
adjustment loop does not — that asymmetry is the bug.

## gdb (release) — why it is DoS, not the "write primitive"

```
#0 std::__shared_ptr<tex::Box>::get (this=0x0)   shared_ptr_base.h:1752   mov (%rax),%rax ; rax=0
#3 MatrixAtom::recalculateLine  atom_matrix.cpp:315
#4 MatrixAtom::createBox        atom_matrix.cpp:526
si_addr = 0x0     rip in valid code
```

At `:315`, `boxarr[j]` (OOB) reads back **NULL**, then `boxarr[j][0]->_type` dereferences
it → **null-deref at `0x0`**, `rip` in valid code. The `height[j] += ex` write two lines
down is **never reached** — the gating read faults first. 20/20 deterministic; spans
9/50/200/900 all fault at `:315`, never at the write. So the earlier "controlled heap
write / CWE-787" framing is wrong in practice: it is a null-deref DoS. (ASan reports it as
a heap-buffer-overflow READ of size 8 at `:315`, alloc at `:441` — same site.)

## The two-line variant is the same bug

`\begin{array}{c}\multirow{5}{*}{x}\end{array}\n\begin{matrix}\multirow{20}{*}{x}\end{matrix}`
crashes at the **identical** site (`:315`, `this=0x0`, `rip` identical). The first array's
multirow faults during the render walk; the second formula never renders. Not distinct.

## Fix

```cpp
const int mr = std::min(m->_i + m->_n, rows);   // clamp span to real row count
for (int j = std::max(m->_i, 0); j < mr; j++) { ... }
```
(Already present in the local working tree; needs upstreaming.) Optionally also reject
`_i + _n > rows` at construction. `kMaxArrayRowSpan = 1000` bounds `|n|` but does **not**
tie it to `rows`, so it does not fix the overrun on its own.
