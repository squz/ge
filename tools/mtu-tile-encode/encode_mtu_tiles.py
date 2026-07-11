#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""MTU-guaranteed keyframe tile encode experiment.

Mechanism (the thing we want in production later):
  1. Partition the frame into fixed tiles (default 256x256).
  2. For each tile, encode an H.264 IDR and measure annex-B size.
  3. If size > BUDGET, raise lossiness (CRF up / bitrate down) and retry.
  4. Binary-search the mildest loss that still fits BUDGET.
  5. If even max lossiness exceeds BUDGET, mark FAIL (unit cannot guarantee).

This is a hard guarantee on *encoded unit* size, not P95. Quality is whatever
falls out — acceptable for the dev player if still usable.

Usage:
  ./encode_mtu_tiles.py --input /path/to.png --out /tmp/mtu-exp
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

# App payload budget for one QUIC datagram after pigeon/AEAD/headers.
# Pigeon MaxDatagramPayload is 1200; leave margin for channel framing + AEAD.
DEFAULT_BUDGET = 1000
DEFAULT_TILE = 256
# libx264 CRF: lower = less lossy. 18 ≈ high quality; 40+ very soft; 51 max.
CRF_MIN = 18
CRF_MAX = 51


@dataclass
class TileResult:
    tile_id: int
    x: int
    y: int
    w: int
    h: int
    size: int
    crf: int
    ok: bool
    attempts: int


def run(cmd: list[str]) -> None:
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def encode_key_h264(png: Path, out_h264: Path, crf: int) -> int:
    """Single IDR via libx264; return annex-B byte size."""
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


def encode_under_budget(
    png: Path, out_h264: Path, budget: int, crf_min: int, crf_max: int
) -> tuple[int, int, int, bool]:
    """Mildest CRF in [crf_min, crf_max] with size <= budget.

    Returns (size, crf, attempts, ok).
    Binary search favors higher quality (lower CRF) while staying under budget.
    """
    # Quick reject: even max loss may not fit.
    size_max = encode_key_h264(png, out_h264, crf_max)
    attempts = 1
    if size_max > budget:
        return size_max, crf_max, attempts, False

    # size at crf_max fits. Find lowest CRF that still fits.
    lo, hi = crf_min, crf_max  # lo = better quality
    best_crf, best_size = crf_max, size_max
    # Check mild end first
    size_lo = encode_key_h264(png, out_h264, lo)
    attempts += 1
    if size_lo <= budget:
        return size_lo, lo, attempts, True

    while lo + 1 < hi:
        mid = (lo + hi) // 2
        sz = encode_key_h264(png, out_h264, mid)
        attempts += 1
        if sz <= budget:
            best_crf, best_size = mid, sz
            hi = mid  # try milder (lower CRF)
        else:
            lo = mid
    # materialize best
    if best_crf != hi or out_h264.stat().st_size != best_size:
        best_size = encode_key_h264(png, out_h264, best_crf)
        attempts += 1
    return best_size, best_crf, attempts, best_size <= budget


def pad_to_grid(im: Image.Image, tile: int) -> Image.Image:
    w, h = im.size
    nw = math.ceil(w / tile) * tile
    nh = math.ceil(h / tile) * tile
    if (nw, nh) == (w, h):
        return im
    canvas = Image.new("RGB", (nw, nh), (0, 0, 0))
    canvas.paste(im, (0, 0))
    return canvas


def decode_h264_to_png(h264: Path, png: Path) -> None:
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET)
    ap.add_argument("--tile", type=int, default=DEFAULT_TILE)
    ap.add_argument("--crf-min", type=int, default=CRF_MIN)
    ap.add_argument("--crf-max", type=int, default=CRF_MAX)
    ap.add_argument(
        "--try-full",
        action="store_true",
        help="Also attempt full-frame under budget (usually fails)",
    )
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    tiles_dir = args.out / "tiles"
    tiles_dir.mkdir(exist_ok=True)

    im = Image.open(args.input).convert("RGB")
    orig_w, orig_h = im.size
    padded = pad_to_grid(im, args.tile)
    pad_path = args.out / "source_padded.png"
    padded.save(pad_path)
    pw, ph = padded.size
    cols, rows = pw // args.tile, ph // args.tile

    results: list[TileResult] = []
    mosaic = Image.new("RGB", (pw, ph), (0, 0, 0))

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        for ty in range(rows):
            for tx in range(cols):
                tid = ty * cols + tx
                x, y = tx * args.tile, ty * args.tile
                tile_im = padded.crop((x, y, x + args.tile, y + args.tile))
                tile_png = td_path / f"t{tid:03d}.png"
                tile_im.save(tile_png)
                h264 = tiles_dir / f"t{tid:03d}.h264"
                size, crf, attempts, ok = encode_under_budget(
                    tile_png, h264, args.budget, args.crf_min, args.crf_max
                )
                results.append(
                    TileResult(tid, x, y, args.tile, args.tile, size, crf, ok, attempts)
                )
                if ok:
                    dec = td_path / f"t{tid:03d}_dec.png"
                    try:
                        decode_h264_to_png(h264, dec)
                        mosaic.paste(Image.open(dec).convert("RGB"), (x, y))
                    except Exception:
                        pass  # leave black on decode failure

        full_info = None
        if args.try_full:
            full_h264 = args.out / "full_key.h264"
            size, crf, attempts, ok = encode_under_budget(
                pad_path, full_h264, args.budget, args.crf_min, args.crf_max
            )
            full_info = {
                "size": size,
                "crf": crf,
                "attempts": attempts,
                "ok": ok,
                "budget": args.budget,
            }

    mosaic_path = args.out / "mosaic_mtu.png"
    # crop pad off for display compare
    mosaic.crop((0, 0, orig_w, orig_h)).save(mosaic_path)
    Image.open(args.input).convert("RGB").save(args.out / "source.png")

    sizes = [r.size for r in results]
    crfs = [r.crf for r in results]
    oks = sum(1 for r in results if r.ok)
    summary = {
        "mechanism": "per-tile IDR + binary-search CRF until size <= budget",
        "budget_bytes": args.budget,
        "tile": args.tile,
        "source": {"w": orig_w, "h": orig_h},
        "padded": {"w": pw, "h": ph, "cols": cols, "rows": rows, "n_tiles": len(results)},
        "guarantee": {
            "all_fit": oks == len(results),
            "fit_count": oks,
            "fail_count": len(results) - oks,
        },
        "size": {
            "min": min(sizes),
            "p50": int(statistics.median(sizes)),
            "p95": sorted(sizes)[max(0, math.ceil(0.95 * len(sizes)) - 1)],
            "max": max(sizes),
            "sum": sum(sizes),
            "mean": int(statistics.mean(sizes)),
        },
        "crf": {
            "min": min(crfs),
            "p50": int(statistics.median(crfs)),
            "p95": sorted(crfs)[max(0, math.ceil(0.95 * len(crfs)) - 1)],
            "max": max(crfs),
            "mean": statistics.mean(crfs),
        },
        "full_frame": full_info,
        "artifacts": {
            "mosaic": str(mosaic_path),
            "source": str(args.out / "source.png"),
            "tiles_dir": str(tiles_dir),
        },
    }
    report_path = args.out / "report.json"
    report_path.write_text(
        json.dumps({"summary": summary, "tiles": [asdict(r) for r in results]}, indent=2)
        + "\n"
    )

    print(json.dumps(summary, indent=2))
    print(f"report: {report_path}", file=sys.stderr)
    print(f"mosaic: {mosaic_path}", file=sys.stderr)
    return 0 if summary["guarantee"]["all_fit"] else 2


if __name__ == "__main__":
    sys.exit(main())
