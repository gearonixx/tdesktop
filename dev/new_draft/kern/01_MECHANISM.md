# Mechanism — byte-level walkthrough of `_unitConversions[-1](env)`

Supplements §2–§3 of the [dossier](00_DOSSIER.md) with the exact arithmetic and ABI.

## The index

```cpp
// enums.h
enum class UnitType : i8 { em, ex, pixel, point, pica, mu, cm, mm, in, sp, pt, dd, cc, x8, none = -1 };
//                         0   1   2      3      4     5   6   7   8   9   10  11  12  13   -1
// utils.h
using i8 = std::int8_t;

// atom_space.h:45
_unitConversions[static_cast<i8>(unit)](env)
```

For `unit == none`: `static_cast<i8>(none) == (int8_t)(-1) == -1`. C++ array subscript is
pointer arithmetic: `_unitConversions[-1] == *(_unitConversions + (-1))`. With
`sizeof(std::function<float(const Environment&)>) == 32` on libstdc++ x86-64, this
dereferences the 32 bytes at `&_unitConversions - 32`.

Valid indices are `0..13` (the array has **14** initialized `std::function` entries, one
per non-negative enum value, `unit_conversion.cpp`). `-1` is one element **below** the
array — into whatever global the linker placed adjacent in `.data`/`.data.rel.ro`.

## The array

```cpp
// unit_conversion.cpp — 14 lambdas, in enum order:
const function<float(const Environment&)> SpaceAtom::_unitConversions[]{
  /*em*/  [](const Environment& e){ return e.getTeXFont()->getEM(e.getStyle()); },
  /*ex*/  ...
  /*pixel*/ ...
  ... 14 total ...
};
```

Each element is a real `std::function` wrapping a lambda. Element `[-1]` is **not** part
of this array; it is a foreign object reinterpreted as a `std::function`.

## The call ABI (libstdc++)

`std::function` memory layout:

```
struct std::function<R(Args...)> {           // 32 bytes
  _Any_data      _M_functor;   // offset 0x00, 16 bytes (union: inline storage or pointer)
  _Manager_type  _M_manager;   // offset 0x10, 8 bytes  (function pointer; null ⇒ empty)
  _Invoker_type  _M_invoker;   // offset 0x18, 8 bytes  (function pointer; the actual call)
};
```

`operator()(Args... args)`:
```cpp
if (_M_empty())                       // _M_empty()  ==  (_M_manager == nullptr)
  __throw_bad_function_call();
return _M_invoker(_M_functor, std::forward<Args>(args)...);
```

So invoking `_unitConversions[-1](env)` performs, on the 32 bytes of the neighbouring
global `G = _unitConversions - 32`:

1. read `G._M_manager` (`[G + 0x10]`); if null, throw `bad_function_call` (a C++
   exception — would unwind, not crash wild). In the observed cases it is **non-null**, so:
2. read `G._M_invoker` (`[G + 0x18]`) and **call it** as
   `float (*)(const _Any_data&, const Environment&)`,
3. with `arg0 = &G._M_functor` (`== &G`, since `_M_functor` is at offset 0) and
   `arg1 = env`.

### Mapping to the observed registers (real client)

| ABI role | Register | Observed | Meaning |
|---|---|---|---|
| call target `_M_invoker` | `rip` | `0x5555_74ca_9e00` (varies) | pointer read from `[G + 0x18]` → jumped to |
| `arg0 = &G._M_functor` | `rdi` | `0x5555_625f_f060` (constant) | address of `G` = `_unitConversions[-1]` (fixed global) |
| `arg1 = env` | `rsi` | `0x5555_6424_2f70` (varies) | the live `Environment&` |

`rdi` constant ⇒ `G` is a fixed global. `rip` in the heap and varying ⇒ `G`'s
`_M_invoker` slot holds a heap pointer that moves with ASLR. Hence: a deterministic
indirect call whose target is data, landing in the heap.

## Why the harness said `0x0` but the client jumps to the heap

The only variable is **which global is `G`** and what its bytes are:

- In the standalone harness (`build_plain`, `build_a`), `G` happens to be `tex::PI`; its
  `[+0x18]` bytes are `0x0`, so `rip = 0` (immediate null-exec fault).
- In the full Telegram binary the neighbour is a different object (`rdi = 0x5555625ff060`)
  whose `[+0x18]` bytes are a **heap pointer**, so `rip` = heap and control transfers
  into live, potentially attacker-adjacent memory.

Same defect, different neighbour. Do not let the harness's tidy `0x0` crash understate the
shipping behavior — always read this bug from the real-client dumps.

## Sibling sink

`getSize` (`atom_space.h:49`) has the identical `_unitConversions[static_cast<i8>(unit)]`
pattern and is used by other length-consuming macros. Any path that can deliver
`UnitType::none` (or any out-of-range unit) to `getSize` is equally affected. The fix must
cover both.
