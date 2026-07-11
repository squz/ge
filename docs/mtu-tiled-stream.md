# Tiled H.264 stream (🎯T151)

## Goal

Deliver video as **independent tile AUs** so each loss domain is a spatial
cell, not a multi-hundred-KB full-frame key. Tiles stay the encode-side unit
for the live stream and for the pigeon datagram path.

## Quality bar (locked 2026-07-11)

Lab ladder on a Jevons (iPad) globe screenshot, libx264 CRF stills:

| CRF | Call |
|-----|------|
| 32 | Perfect |
| **36** | **Sweet spot** — faint loss, not meaningful |
| 40 | Noticeable artifacts |

Product path is **VideoToolbox**, not CRF. Calibrate VT bitrate/quality to the
**CRF 36 look**. Offline ladder: `tools/mtu-tile-encode/`.

## Size policy (locked with pigeon auto-fragment)

**Stick with tiles.** Do **not** hard-guarantee every AU ≤ one MTU.

1. Choose tile edge (and bitrate) so **most** tiles fit a single DATAGRAM-sized
   payload under normal compression (statistical fit — e.g. typical P-tiles;
   keys may spike).
2. **Allow a minority of tiles to exceed that envelope.** On pigeon, the
   transport **auto-fragments** on send; the app receives **fragments as they
   arrive** (no auto-unfragment). The application decides partial delivery,
   miss maps, and timeouts.
3. **Do not** pass-2-crush or blank solely to force a hard MTU fit on every
   tile — that invents quality loss (especially on localhost WebSocket) for a
   limit the path no longer requires as a hard envelope.
4. Never feed torn/partial **H.264 NALs** to the decoder: only complete tile
   AUs (or complete app-defined fragment groups reassembled by the app).

### Path defaults

| Path | Guidance |
|------|----------|
| **WebSocket / LAN (today)** | No bandwidth reason to crush; large soft budget is fine. |
| **Pigeon datagrams (next)** | Size for *most* tiles ≤ ~1 datagram payload; rely on pigeon frag for the rest. |

`GE_STREAM_MTU` remains an optional *soft target / telemetry hint*, not a
must-blank ceiling.

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

`blank` remains for true encode failure / explicit skip, not “slightly over MTU”.

## Controls

| Env | Default | Meaning |
|-----|---------|---------|
| `GE_STREAM_TILES` | `1` | `0` = legacy full-frame encoder |
| `GE_STREAM_MTU` | `16384` | Soft size hint (pass-2 disabled for hard fit); set lower only for experiments |
| `GE_STREAM_TILE` | `512` | Preferred tile edge (px); prefer divisors of frame size |
| `GE_STREAM_BPS` | `8000000` | Total average bitrate across all tiles |

## Relation to pigeon (T11.3)

- **Tiles** = isolatable decode units (spatial loss domains).  
- **Pigeon auto-fragment** = wire packing for the occasional oversized tile.  
- **App** = fragment arrival, partial policy, timeouts (no transport reassembly).  
- Spyder stays off the data path post-pair; QR/nonce remains spyder (T93).
