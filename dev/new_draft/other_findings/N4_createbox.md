# Finding #4 (N4) — ragged-row OOB in `MatrixAtom::createBox` (DoS, hardened)

**Trigger:** `\begin{tabular}0\\&\hline` (25-byte minimal PoC)
**Site:** `src/atom/atom_matrix.cpp:595` (ASan reported `:603` on a differently-built tree;
same hline-adjacency check)
**Class:** OOB read of an `sptr<Atom>` from a ragged row. In a hardened build this is
**caught by `_GLIBCXX_ASSERTIONS`** → clean `abort()`. **DoS (mitigated).** Live.

## Root cause

The `hline` branch checks the cell directly above for hline adjacency:
```cpp
if (i >= 1 && dynamic_cast<HlineAtom*>(_matrix->_array[i - 1][j].get()) != nullptr)
```
`_array` rows are ragged (a short/empty row above a wider one). `j` is driven by the wider
current row, so `_array[i-1][j]` indexes past the end of the shorter previous row.

## gdb (release) — the hardening turns it into a safe abort

```
stderr: stl_vector.h:1253: …vector<shared_ptr<tex::Atom>>::operator[](size_type):
        Assertion '__n < this->size()' failed.
#3 std::__glibcxx_assert_fail
#4 std::vector<std::shared_ptr<tex::Atom>>::operator[] (this=…, __n=1)
#5 tex::MatrixAtom::createBox                                   atom_matrix.cpp:595
SIGABRT, rip in libc
```

Because the access goes through `std::vector::operator[]` (not a raw array), `_GLIBCXX_ASSERTIONS`
bounds-checks it and calls `abort()` **before** any wild read — the safest possible
outcome. Contrast the multirow bug, which uses **raw** `new[]` arrays and therefore gets no
such protection.

## Fix

Bound the column into the previous row:
```cpp
auto& prev = _matrix->_array[i - 1];
if (i >= 1 && j < (int)prev.size()
    && dynamic_cast<HlineAtom*>(prev[j].get()) != nullptr) { ... }
```

## Note

This is the one finding *proven* to reach the real client as a MicroTeX crash: dump
`9fb12b82` (2026-07-03 07:40) carries the exact assertion text `__n < this->size()` for
`vector<shared_ptr<tex::Atom>>` — a `SIGABRT` in libc from this site. It is the attribution
anchor that confirms the testing session was exercising MicroTeX.
