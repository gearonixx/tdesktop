# Proposed Fix (NOT applied)

This draft does **not** modify source. Below is the recommended patch and test plan for
the maintainers.

## Primary fix — bounds-check the sink

`src/atom/atom_space.h`, both `getFactor` (line 44) and `getSize` (line 48):

```diff
   /** Get the scale factor from the given unit and environment */
   inline static float getFactor(UnitType unit, const Environment& env) {
-    return _unitConversions[static_cast<i8>(unit)](env);
+    const auto i = static_cast<i8>(unit);
+    // UnitType::none == -1 (and any out-of-range unit) would index before/after the
+    // fixed _unitConversions[] array and invoke a foreign object as a std::function.
+    // \kern with no length yields UnitType::none -> reject and contribute no space.
+    if (i < 0 || i >= _unitConversionsCount) return 0.f;
+    return _unitConversions[i](env);
   }

   inline static float getSize(UnitType unit, float size, const Environment& env) {
-    return _unitConversions[static_cast<i8>(unit)](env) * size;
+    const auto i = static_cast<i8>(unit);
+    if (i < 0 || i >= _unitConversionsCount) return 0.f;
+    return _unitConversions[i](env) * size;
   }
```

### Bound to use

`_unitConversions` has **14** entries; the existing `_unitsCount` is the size of the
*names* table `_units[]` (16 aliases), which is a **different** number. Do **not** reuse
`_unitsCount`. Add an explicit count next to the array:

```cpp
// src/atom/unit_conversion.cpp
const i32 SpaceAtom::_unitConversionsCount =
    sizeof(_unitConversions) / sizeof(_unitConversions[0]);   // == 14
```
```cpp
// src/atom/atom_space.h (class SpaceAtom, near _unitConversions)
static const i32 _unitConversionsCount;
```

## Defense in depth — reject `none` at the macro

`src/core/macro_impl.cpp`, `macro(kern)` (line 12): a missing length should not propagate a
`none` unit into a `SpaceAtom` at all.

```diff
 macro(kern) {
   auto[unit, value] = tp.getLength();
+  if (unit == UnitType::none) { unit = UnitType::pixel; value = 0.f; } // no length -> zero-width
   return sptrOf<SpaceAtom>(unit, value, 0.f, 0.f);
 }
```

(The parser returns `{UnitType::none, -1.f}` from `getLength()` at end-of-input,
`parser.cpp:694`. Any other macro consuming `getLength()` benefits from the sink fix
regardless.)

## Why not just clamp `none` to `pixel` in `getFactor`

Returning `0.f` for an invalid unit is the safest, most local change and matches the
existing `getUnit` fallback (`atom_space.cpp:13`, unknown unit → `pixel`). Clamping to a
real conversion would silently render a bogus space; `0.f` renders nothing, which is the
correct behavior for a malformed `\kern`.

## Test plan

1. **Regression PoC:** `./build_plain/plain '\kern'` must exit 0 after the fix (currently
   SIGSEGV). Same for `$\kern$` in the real client (no minidump written).
2. **Non-regression:** valid spaces still render — `\kern 3pt`, `\hspace{1cm}`,
   `\,`/`\;`/`\quad`, `\rule{1pt}{1pt}` — visually unchanged.
3. **Sibling sink:** exercise any macro that routes a length through `getSize` with a
   malformed/absent unit; confirm no OOB.
4. **Fuzz delta:** re-run the AFL corpus that produced N3; the `getFactor`
   global-buffer-overflow signature should disappear.

## Scope

Fixes finding #1 (`\kern` control-flow hijack) and its `getFactor`/`getSize` OOB class. It
does **not** address the other DoS findings (multirow, N2, N4, null-deref cluster) — those
need their own guards; see [`../other_findings/`](../other_findings/).
