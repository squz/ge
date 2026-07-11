#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""MTU tile encode — P99-sized grid + aggressive tail pass + blank failures.

Strategy (dev-player packet guarantee):
  1. Pick the largest tile edge such that, under *default* compression,
     P99(tile AU size) <= BUDGET. (Statistical sizing, not worst-case.)
  2. First pass: encode every tile at default quality.
  3. Second pass: only tiles with size > BUDGET — aggressively re-encode
     (binary-search higher loss until fit or max loss).
  4. Anything still over BUDGET after pass 2 is a *blank tile* (not sent /
     delivered as empty region). Guarantee: every *delivered* packet <= BUDGET.

Usage:
  ./encode_mtu_tiles_v2.py --input frame.png --out /tmp/mtu-v2
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

from PIL import Image

DEFAULT_BUDGET = 1000
# Dev-player target from Jevons globe keyframe ladder (2026-07): CRF 36 is
# the sweet spot (32 = perfect, 40 = noticeable artifacts). Auto tile at that
# quality is 64×64 under a 1000 B budget. MTU hard-cap via P99 tile sizing +
# pass-2 + blank. (libx264 lab dial; product path is VideoToolbox calibrated
# to this look, not this CRF number.)
DEFAULT_CRF = 36
AGGRESSIVE_CRF_MAX = 51
# Prefer larger tiles when P99 fits; search descending.
TILE_CANDIDATES = (256, 192, 160, 128, 96, 80, 64)


@dataclass
class TileResult:
    tile_id: int
    x: int
    y: int
    size_pass1: int
    size_final: int
    crf_final: int
    pass1_over: bool
    blank: bool  # True => do not deliver; paint empty


def run(cmd: list[str]) -> None:
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def encode_key(png: Path, out_h264: Path, crf: int) -> int:
    run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(png),
            "-frames:v",
            "1",
            "-c:v",
            "libx264",
            "-profile:v",
            "high",
            "-pix_fmt",
            "yuv420p",
            "-x264-params",
            "keyint=1:min-keyint=1:scenecut=0",
            "-crf",
            str(crf),
            "-preset",
            "veryfast",
            "-an",
            "-f",
            "h264",
            str(out_h264),
        ]
    )
    return out_h264.stat().st_size


def pct(sorted_vals: list[int], p: float) -> int:
    if not sorted_vals:
        return 0
    idx = max(0, math.ceil(p / 100.0 * len(sorted_vals)) - 1)
    return sorted_vals[min(idx, len(sorted_vals) - 1)]


def pad_to_grid(im: Image.Image, tile: int) -> Image.Image:
    w, h = im.size
    nw = math.ceil(w / tile) * tile
    nh = math.ceil(h / tile) * tile
    if (nw, nh) == (w, h):
        return im.convert("RGB") if im.mode != "RGB" else im
    canvas = Image.new("RGB", (nw, nh), (0, 0, 0))
    canvas.paste(im.convert("RGB"), (0, 0))
    return canvas


def decode_h264(h264: Path, png: Path) -> None:
    run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "h264",
            "-i",
            str(h264),
            "-frames:v",
            "1",
            str(png),
        ]
    )


def aggressive_fit(png: Path, out_h264: Path, budget: int, crf_lo: int, crf_hi: int) -> tuple[int, int, bool]:
    """Raise CRF from just above default toward crf_hi until size <= budget.

    crf_lo is the default (already failed). Search (crf_lo, crf_hi].
    """
    size_hi = encode_key(png, out_h264, crf_hi)
    if size_hi > budget:
        return size_hi, crf_hi, False

    lo, hi = crf_lo, crf_hi  # need > lo
    best_crf, best_size = crf_hi, size_hi
    # invariant: hi fits; find mildest that fits
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        sz = encode_key(png, out_h264, mid)
        if sz <= budget:
            best_crf, best_size = mid, sz
            hi = mid
        else:
            lo = mid
    if out_h264.stat().st_size != best_size or best_crf != hi:
        best_size = encode_key(png, out_h264, best_crf)
    return best_size, best_crf, best_size <= budget


def measure_p99(padded: Image.Image, tile: int, crf: int, tmp: Path) -> tuple[int, list[int]]:
    pw, ph = padded.size
    cols, rows = pw // tile, ph // tile
    sizes: list[int] = []
    for ty in range(rows):
        for tx in range(cols):
            x, y = tx * tile, ty * tile
            crop = padded.crop((x, y, x + tile, y + tile))
            png = tmp / f"m_{tile}_{tx}_{ty}.png"
            h264 = tmp / f"m_{tile}_{tx}_{ty}.h264"
            crop.save(png)
            sizes.append(encode_key(png, h264, crf))
    sizes_sorted = sorted(sizes)
    return pct(sizes_sorted, 99), sizes


def choose_tile(
    padded_cache: dict[int, Image.Image],
    im: Image.Image,
    candidates: tuple[int, ...],
    budget: int,
    default_crf: int,
    tmp: Path,
) -> tuple[int, int, list[int]]:
    """Largest tile with default-CRF P99 <= budget."""
    for tile in sorted(candidates, reverse=True):
        if tile not in padded_cache:
            padded_cache[tile] = pad_to_grid(im, tile)
        p99, sizes = measure_p99(padded_cache[tile], tile, default_crf, tmp)
        print(
            f"  probe tile={tile} n={len(sizes)} p99={p99} max={max(sizes)} "
            f"budget={budget} => {'OK' if p99 <= budget else 'too big'}",
            file=sys.stderr,
        )
        if p99 <= budget:
            return tile, p99, sizes
    # fallback: smallest candidate even if P99 over
    tile = min(candidates)
    if tile not in padded_cache:
        padded_cache[tile] = pad_to_grid(im, tile)
    p99, sizes = measure_p99(padded_cache[tile], tile, default_crf, tmp)
    return tile, p99, sizes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    ap.add_argument("--default-crf", type=int, default=DEFAULT_CRF)
    ap.add_argument("--crf-max", type=int, default=AGGRESSIVE_CRF_MAX)
    ap.add_argument(
        "--tile",
        type=int,
        default=0,
        help="Force tile edge (skip P99 search). 0 = auto.",
    )
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tiles_dir = args.out / "tiles"
    tiles_dir.mkdir(exist_ok=True)

    im = Image.open(args.input).convert("RGB")
    orig_w, orig_h = im.size
    padded_cache: dict[int, Image.Image] = {}

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        print("pass0: choose tile so default P99 <= budget", file=sys.stderr)
        if args.tile > 0:
            tile = args.tile
            padded = pad_to_grid(im, tile)
            p99, probe_sizes = measure_p99(padded, tile, args.default_crf, tmp)
            print(
                f"  forced tile={tile} p99={p99} max={max(probe_sizes)}",
                file=sys.stderr,
            )
        else:
            tile, p99, probe_sizes = choose_tile(
                padded_cache,
                im,
                TILE_CANDIDATES,
                args.budget,
                args.default_crf,
                tmp,
            )
            padded = padded_cache[tile]

        pw, ph = padded.size
        cols, rows = pw // tile, ph // tile
        padded.save(args.out / "source_padded.png")
        im.save(args.out / "source.png")

        results: list[TileResult] = []
        mosaic = Image.new("RGB", (pw, ph), (0, 0, 0))
        n_pass2 = 0
        n_blank = 0

        print(
            f"pass1: default CRF={args.default_crf} on {cols}x{rows}={cols*rows} tiles",
            file=sys.stderr,
        )
        tile_pngs: list[Path] = []
        for ty in range(rows):
            for tx in range(cols):
                tid = ty * cols + tx
                x, y = tx * tile, ty * tile
                crop = padded.crop((x, y, x + tile, y + tile))
                png = tmp / f"t{tid:03d}.png"
                crop.save(png)
                tile_pngs.append(png)
                h264 = tiles_dir / f"t{tid:03d}.h264"
                sz1 = encode_key(png, h264, args.default_crf)
                over = sz1 > args.budget
                results.append(
                    TileResult(
                        tile_id=tid,
                        x=x,
                        y=y,
                        size_pass1=sz1,
                        size_final=sz1,
                        crf_final=args.default_crf,
                        pass1_over=over,
                        blank=False,
                    )
                )

        print("pass2: aggressive re-encode for oversize only", file=sys.stderr)
        for r, png in zip(results, tile_pngs):
            h264 = tiles_dir / f"t{r.tile_id:03d}.h264"
            if not r.pass1_over:
                # decode for mosaic
                dec = tmp / f"d{r.tile_id:03d}.png"
                try:
                    decode_h264(h264, dec)
                    mosaic.paste(Image.open(dec).convert("RGB"), (r.x, r.y))
                except Exception:
                    pass
                continue
            n_pass2 += 1
            sz, crf, ok = aggressive_fit(
                png, h264, args.budget, args.default_crf, args.crf_max
            )
            r.size_final = sz
            r.crf_final = crf
            if not ok:
                r.blank = True
                n_blank += 1
                h264.unlink(missing_ok=True)
                # leave mosaic black for blank
            else:
                dec = tmp / f"d{r.tile_id:03d}.png"
                try:
                    decode_h264(h264, dec)
                    mosaic.paste(Image.open(dec).convert("RGB"), (r.x, r.y))
                except Exception:
                    pass

    mosaic.crop((0, 0, orig_w, orig_h)).save(args.out / "mosaic_mtu.png")

    delivered = [r for r in results if not r.blank]
    d_sizes = [r.size_final for r in delivered]
    p1_sizes = [r.size_pass1 for r in results]
    over_budget_delivered = sum(1 for s in d_sizes if s > args.budget)

    summary = {
        "strategy": "P99-sized tiles + aggressive pass-2 tail + blank failures",
        "budget_bytes": args.budget,
        "default_crf": args.default_crf,
        "crf_max": args.crf_max,
        "tile": tile,
        "source": {"w": orig_w, "h": orig_h},
        "grid": {"w": pw, "h": ph, "cols": cols, "rows": rows, "n_tiles": len(results)},
        "pass0_p99_default": p99,
        "pass0_p99_fits_budget": p99 <= args.budget,
        "pass1": {
            "over_count": sum(1 for r in results if r.pass1_over),
            "over_frac": sum(1 for r in results if r.pass1_over) / len(results),
            "size_p50": pct(sorted(p1_sizes), 50),
            "size_p95": pct(sorted(p1_sizes), 95),
            "size_p99": pct(sorted(p1_sizes), 99),
            "size_max": max(p1_sizes),
        },
        "pass2": {"reencoded": n_pass2, "blanked": n_blank},
        "guarantee": {
            "all_delivered_packets_le_budget": over_budget_delivered == 0,
            "delivered": len(delivered),
            "blank": n_blank,
            "blank_frac": n_blank / len(results),
        },
        "delivered_size": {
            "min": min(d_sizes) if d_sizes else 0,
            "p50": pct(sorted(d_sizes), 50) if d_sizes else 0,
            "p95": pct(sorted(d_sizes), 95) if d_sizes else 0,
            "p99": pct(sorted(d_sizes), 99) if d_sizes else 0,
            "max": max(d_sizes) if d_sizes else 0,
            "sum": sum(d_sizes),
        },
        "artifacts": {
            "mosaic": str(args.out / "mosaic_mtu.png"),
            "source": str(args.out / "source.png"),
            "report": str(args.out / "report.json"),
        },
    }
    (args.out / "report.json").write_text(
        json.dumps({"summary": summary, "tiles": [asdict(r) for r in results]}, indent=2)
        + "\n"
    )
    print(json.dumps(summary, indent=2))
    return 0 if summary["guarantee"]["all_delivered_packets_le_budget"] else 2


if __name__ == "__main__":
    sys.exit(main())
