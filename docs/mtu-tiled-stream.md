# MTU-capped tiled H.264 stream (🎯T151)

## Goal

Deliver every **on-wire video unit** within a hard application budget
(**1000 bytes** of H.264 AU payload after GE2V headers) so the stream can ride
QUIC/pigeon datagrams without all-or-nothing multi-hundred-KB keyframes.

## Quality bar (locked 2026-07-11)

Lab ladder on a Jevons (iPad) globe screenshot, libx264 CRF stills, auto tile:

| CRF | Call |
|-----|------|
| 32 | Perfect |
| **36** | **Sweet spot** — faint loss, not meaningful |
| 40 | Noticeable artifacts |

Product path is **VideoToolbox**, not CRF. Calibrate VT bitrate/quality to the
**CRF 36 look**. Offline ladder: `tools/mtu-tile-encode/`.

## Policy

1. Partition the encode framebuffer into a grid of independent tiles.
2. Choose tile edge so default compression keeps sizes under budget when
   practical; grow tile edge if VT session count would explode.
3. Encode each tile (VT session per tile).
4. If AU > budget → pass-2 harder encode; if still over → **blank** (do not
   send; player keeps prior/empty region).
5. Never feed torn/partial NALs to the decoder.

## Wire (GE2V)

Legacy (untiled):

```
flags u8 | seq u32 | avcc…
flags bit0 = keyframe
```

Tiled (`flags & kVideoFlagTiled`):

```
flags u8 | frame_seq u32 | tile_id u16 | cols u8 | rows u8
         | frame_w u16 | frame_h u16 | tile_edge u16
         | [avcc… if not blank]
// fixed tiled header = 15 bytes; flags bit0=key bit1=tiled bit2=blank
// tile origin = (tile_id % cols) * tile_edge, (tile_id / cols) * tile_edge
```

## Controls

| Env | Default | Meaning |
|-----|---------|---------|
| `GE_STREAM_TILES` | `1` | `0` = legacy full-frame encoder |
| `GE_STREAM_MTU` | `1000` | Max AU bytes per delivered tile |
| `GE_STREAM_TILE` | `64` | Preferred tile edge (px) |
| `GE_STREAM_BPS` | `6000000` | Total average bitrate across all tiles |

## Relation to pigeon (T11.3)

Tiles are the encode-side answer to datagram size. Spyder stays off the data
path post-pair; pairing QR remains spyder (T93).
