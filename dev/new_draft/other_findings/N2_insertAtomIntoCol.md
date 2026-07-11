# Finding #3 (N2) — OOB in `ArrayFormula::insertAtomIntoCol` (DoS)

**Trigger:** `\begin{cases}\\&\end{cases}` (16-byte minimal: `\begin{cases}\\&`)
**Site:** `src/core/formula.cpp:214`
**Class:** heap-buffer-overflow, OOB read during `std::vector` reallocation. **DoS**
(heap-corruption risk, unproven). Live.

## Root cause

```cpp
void ArrayFormula::insertAtomIntoCol(int col, const sptr<Atom>& atom) {
  _col++;
  for (size_t j = 0; j < _row; j++) {
    auto it = _array[j].begin();
    _array[j].insert(it + col, atom);   // :214  it + col can be past end()
  }
}
```

`col` is the current column position but is applied to **every prior row**. When an earlier
row is shorter than `col`, `it + col` is an iterator **past `end()`**. `\\` (addRow) leaves
a short/empty row while `_row` still counts it; `&` (addCol) then inserts at a `col`
exceeding that row's length.

## gdb (release)

```
#0 std::__shared_ptr<tex::Atom>::__shared_ptr(&&)  (shared_ptr move)   fault @ 0x555555…c000  (page boundary)
#4 std::__relocate_object_a(__dest=…, __orig=0x55555584c000)           reading source past end
#7 std::vector<…>::_M_realloc_insert                                    vector.tcc:504
#9 tex::ArrayFormula::insertAtomIntoCol (col=3)                         formula.cpp:214
rip in valid code
```

When capacity is exceeded, `_M_realloc_insert` relocates `[position, end())`; with
`position` past `end()` the relocation range is invalid and `__relocate_a` walks from a bad
`__first` off the end of the old buffer → reads across a **page boundary**
(`0x…c000`) → SIGSEGV. `rip` stays in valid libstdc++ code → **data fault, DoS**. The
relocation also *writes* to the destination (shared_ptr move), but the value moved is an
internal `Atom*`, not attacker data, and the corrupted refcounts crash it — no controlled
write demonstrated.

## Fix

Bound the column into each row before inserting:
```cpp
for (size_t j = 0; j < _row; j++) {
  auto& row = _array[j];
  const size_t pos = std::min<size_t>(col < 0 ? 0 : col, row.size());
  row.insert(row.begin() + pos, atom);
}
```
(or reject `col > row.size()`). Same *ragged-row* family as N4 but a different, parse-time
sink; each needs its own guard.
