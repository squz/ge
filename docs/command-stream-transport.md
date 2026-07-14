# Command-Stream Transport — a draw-call rung above video streaming

**Status:** Proposed (spike-pending). Captures a design conversation, 2026-06-29;
revised 2026-07-05 for the negotiated-ladder / spyder-relay model.
**Targets:** 🎯T128 and its subgraph (see *Target subgraph* below).
**Supersedes in spirit:** the original Dawn-wire draw-call player documented in
`docs/ge-remote.md` (now historical).

---

## Summary

ge streams a session to the player as **H.264 video**: the server renders
headless, encodes (VideoToolbox / MediaCodec), and ships frames; the player
decodes and displays. This note proposes an **additional, more advanced rung** on
the streaming path that instead serializes the **render command stream** (sokol
`sg_*` calls plus a small set of higher-level ge verbs) and replays it on the
player's GPU.

The two are **rungs on a negotiated protocol ladder, not a replacement.** Player
and server jointly pick the most advanced protocol they *both* support. **H.264
video is the permanent universal baseline** — every player decodes it in hardware,
every server can encode it, and it is the *only* rung a non-sokol server (or a
player that can't replay) can use. The command stream is the top rung, selected
only when both ends qualify. The broker — **spyder, which is absorbing ged's
role — is agnostic to all of this and only relays bytes**; negotiation is
end-to-end between player and server (see *Protocol negotiation & the relay*).

The bet: **for ge's actual content profile — geometrically simple,
vector/text-heavy 2D/2.5D games served by a sokol-based ge server — the
command-stream rung is the preferred one when available**; video remains for
everything else. Two engine changes make that rung cheap enough to try now where
it wasn't before:

1. **sokol_gfx** collapses the render API to ~159 POD-descriptor C functions.
2. **The T107 dispatch shim** (`g_ge_sg_api`) already routes every `sg_*` call
   through a single installable function table — the exact seam a serializing
   "remoting backend" plugs into, structurally identical to the GLES/VK backends
   we already ship. T109 generalises that seam to interposable capture layers;
   this transport is the production instance of the capture layer.


## Reopen note — 2026-07-14 (OTA full-resolution bandwidth)

The H.264 stream path works on LAN and loopback, but **full-resolution
over-the-air delivery is the practical bottleneck**: tiled encode, soft MTU,
and Android decode costs still leave the Pixel/Wi-Fi path far from a crisp
native frame rate at full res. That is the product driver for reopening 🎯T128.

Command-stream (sokol) economics, restated:

| Phase | Cost shape |
|-------|------------|
| First connect / cold cache | Sizable: shaders + assets (mip-first). **Cacheable** on the player. |
| Steady state / warm reconnect | Near-zero when the scene is stable; scales with *change rate*, not resolution. |
| H.264 (any reconnect) | Flat floor: keyframe cadence × resolution, even for a still scene. |

Spike 🎯T128.2 must measure those three against real Wi-Fi (not only loopback)
before committing to T128.3–T128.6. H.264 remains the permanent baseline rung.

## Product context — this is (mostly) the dev/preview loop

Per 🎯T145, ge's **server streaming path is dev-only**: release (`NDEBUG`) builds
strip streaming + encode, and shipped games render direct-on-device
(`DirectRenderHost`). The streaming server is the *development* modality — drive
the game on your Mac, view and drive it on a device (the "ge Remote" use case in
`docs/ge-remote.md`), brokered by spyder. So the command-stream rung is first and
foremost about a **sharper, lower-latency dev-preview loop**, plus the genuine
option of command-stream anywhere both ends are ge/sokol. There is **no
production-server-cost story here** — the win is a per-session runtime property in
the dev loop (see *Where the win is*).

## Why now — what changed, what didn't

The original cost drivers for "video, not draw calls" were never "how do we
intercept draw calls." They were asset/shader distribution, unbounded
content-dependent bandwidth, the player ceasing to be app-agnostic, and the
maturity of the hardware video path. sokol changes the *interception* problem;
the rest are handled below:

- **Shaders are per-backend bytecode** (`sg_make_shader`). The player already
  advertises its backend (VK vs GLES, picked per device) — so "ship the right
  shader variant" reuses the existing negotiation. Shaders are KB, sent once.
- **Assets must live on the player** (`sg_update_image`/`sg_make_image`). Handled
  by progressive delivery + a content-addressed cache (see *Resource delivery*,
  *Caching*).
- **Bandwidth is content-dependent.** Video's cost is *flat* (a keyframe-cadence ×
  resolution floor paid even on a still scene); command-stream cost *starts near
  zero and rises with per-frame change rate*. There is a crossover; ge's content
  sits well left of it. Reconnection — a first-class ge feature — sharpens this:
  every reconnect forces a fresh IDR keyframe in video; command-stream replay
  reconnects against a warm asset cache for a small delta.
- **Maturity** cuts the other way: the video pipeline is a black box we can't fix
  when VideoToolbox misbehaves; this protocol is ours end-to-end, and its safety
  surface is small (broker-relayed (ged→spyder) stream between *our* server and
  *our* player — "don't crash on a malformed stream from our own encoder," not
  "defend against adversarial GPU commands"; assert-and-abort is acceptable).

## Protocol negotiation & the relay

The transport is chosen by an **end-to-end capability handshake** between player
and server, not by the broker. Each side advertises what it can do; they intersect
and pick the highest common rung:

| Rung | Server must | Player must | Notes |
|---|---|---|---|
| **H.264 video** (baseline) | encode (VideoToolbox / MediaCodec) | decode (hardware) | Permanent floor. The only rung a **non-sokol** server can offer. Always supported both ends. |
| **Command stream** | render through ge's sokol path and serialise it | replay a ge/sokol command stream | Selected only when both ends qualify. Within it, ge **verbs** (SVG/text/image) are a further sub-negotiation, each with a sokol-flatten fallback. |

Three consequences of "most advanced both can operate on":

- **H.264 is retained permanently**, not as a mere churn fallback. spyder cannot
  assume a server uses sokol, some ge servers won't produce a serialisable command
  stream at all, and some players won't replay — so video is the universal interop
  floor by necessity. The command stream is strictly additive on top.
- **The broker is protocol-agnostic — so this needs no broker work.** ged's role
  is moving into **spyder**, which is "solely concerned with streaming bytes." It
  relays whatever the two ends negotiate and never inspects the protocol. Because
  the command-stream rung rides the *same byte pipe* H.264 already uses, adding it
  is a **pure ge-side (player + server) change with zero new spyder-side
  dependency** — the single biggest simplification the spyder-agnostic model buys
  us. (It is also compatible with an E2E-encrypted relay, which *couldn't* inspect
  the protocol anyway; see 🎯T11.)
- **Substrate-agnostic.** Negotiation is in-band and end-to-end, so the transport
  rides whatever byte pipe ge surfaces (ged today, spyder / pigeon tomorrow). This
  design does not block on the ged→spyder migration (🎯T145) completing.

## Where the win is — and isn't

When a session negotiates the **command-stream rung**, the serialising backend
touches no GPU: the server runs game logic, emits `sg_*`, and serialises — a
**null backend, no drawable, no encode** for that session. A command-stream dev
session therefore pays neither the GPU-render nor the H.264-encode cost on the dev
machine, and gains native-resolution quality on the device. That is the real,
modest win: less Mac CPU/fan in the dev loop, no VideoToolbox in the path, crisper
preview.

What it is **not**: a subsystem deletion. Because H.264 is retained (and because
the streaming path is dev-gated per 🎯T145 anyway), **the encoder and the
headless-GPU-render path stay compiled into ge** — a dev-stream server must still
encode for H.264-only players. The GPU-less-server idea only applies to a
hypothetical command-stream-only deployment that declines H.264 players; that is a
niche, not a goal. A real backend also stays available server-side for the offline
rasterising paths that need pixels — `renderToPng` / `renderBatch` (🎯T124
goldens) and `imgdiff`.

## The protocol is a ge API, not the sokol API

The remoting seam sits at **ge's own rendering API**, with the sokol command
stream as the universal substrate beneath it. The protocol has two registers:

- **High-level ge verbs** for cases where the *source representation* is
  dramatically smaller and/or higher-quality than the rasterised result (SVG,
  text, encoded images). A handful.
- **The sokol command stream** as the fallback for everything else (custom
  geometry, custom shaders, procedural textures). This is the vast bulk and
  needs no per-feature design.

### The governing invariant

> **Every ge verb is an optional optimisation with a mandatory sokol-flattening
> fallback.** For any verb a player doesn't understand, the server runs the local
> ge implementation and remotes the resulting sokol commands instead.

So verbs are negotiated per session (nested inside the transport-rung
negotiation), unknown verbs degrade to the frozen sokol base, and the protocol is
forward/backward compatible by construction. This is the same graceful-degradation
property that recurs throughout this design:

- **Whole ladder** — pick the most advanced rung both support; video is the floor.
- **MVP = zero verbs** = the all-flatten case of the verb model.
- **"Everything upfront" = the cap-wide-open case of LOD** (see *Resource
  delivery*).
- **Recipe-known → mip-first → video** is the same descending fallback for assets.

The MVP is therefore *on the path*, not throwaway: it is this architecture with
every optional knob at its trivial setting.

### Cost of moving the seam up (stated honestly)

The sokol seam is *frozen* (changes only on a sokol bump). A ge-API seam is a
*living* protocol that tracks ge's own evolution. Mitigation: most new primitives
just flatten to sokol and need no protocol change, so maintenance is proportional
to optimisation appetite, not API growth — but we do own a growing semantic
surface, versioned at the ge level (advertised in the handshake, like the
backend). Implementation-wise it **composes with** the dispatch shim: a thin
verb-emitting layer sits above the existing `g_ge_sg_api` forwarders; a primitive
either emits a verb or falls through to the sokol shim.

The player becomes "a ge runtime executing ge verbs" — it links the same
lunasvg / FreeType / SDL_image stack the app does, and stays **app-agnostic**: it
knows ge primitives, never the game's logic. (Aligns with 🎯T34: the player is a
regular ge app.)

## Transport model — three priority classes

A command stream is stateful and cumulative (resources created early are
referenced later), so it cannot be treated as homogeneous like a video stream.
Three classes, with different reliability and priority (an end-to-end concern; the
relay stays agnostic to them):

| Class | Contents | Semantics |
|---|---|---|
| **A — ephemeral** | per-frame draw lists (`begin_pass`…`commit`) | droppable under backpressure, latency-critical |
| **B — durable required** | handle creation, coarse texture mips (or whole small assets), dynamic-texture updates, geometry/shaders/pipelines | ordered, **must arrive before the dependent frame**, never dropped |
| **C — durable refinement** | finer texture mips, resources first used in later frames | best-effort, infinitely deferrable, **zero correctness impact** |

- **Epoch barriers** gate frames on **B only**, at *coarse* level: a frame
  unblocks as soon as its textures exist with *something* in them, then sharpens.
  This is what keeps first paint fast. C dropping is never wrong, only blurry.
- **Dependency-ordered delivery gated on the frame-1 working set.** The server
  records frame 1's command list, sees exactly which buffer/image handles its
  draws bind (directly, from `sg_apply_bindings` — no structural understanding of
  the data needed), ships *those* first (coarse for textures), trails the rest.
  Therefore **startup lag = frame-1 working set at coarse detail, decoupled from
  total content size.** A 500 MB-of-textures game starts in ~the time to ship
  frame-1 geometry + shaders + coarse mips, then sharpens.
- Transport shape: B and C want separate streams (QUIC stream priority, or a
  single WebSocket with chunked C-yields-to-B), so a large refinement upload never
  head-of-line-blocks a required byte. The byte pipe is broker-relayed today
  (ged, moving to spyder), with pigeon E2E-encryption on the roadmap (🎯T11).

## Resource delivery

Mechanism: **stable handle + progressive backing.** sokol resources are
handle-identified, so the contents behind a handle can be progressively populated
without the game or the recorded draw commands knowing. Three properties of the
sokol API *enforce* the invisibility (it is not a convention we maintain):

1. **Handle-identified** — the game holds an `sg_image`, never the bytes.
2. **Write-only / no readback** — there is no `sg_read_image`; nothing in the API
   lets a game observe "this texture isn't fully delivered yet."
3. **Usage-tagged** — `sg_image_desc.usage` already distinguishes `immutable` from
   `dynamic`/`stream` (the game fills this in for correctness), giving a *free*
   classifier: immutable-with-data → LOD-eligible asset; dynamic/stream → a live
   texture stream (full rate on B, or the high-churn content that belongs on
   video).

### Texture LOD is the only proxy

Only textures are big enough to gate startup, and they have a free coarse proxy
(a mip). **Meshes/geometry ship whole on B** — they are small (see below) *and*
not safely decimatable at the seam (see *Mesh structure*). LOD here buys
**bandwidth + first-paint, not VRAM** — and **memory is explicitly not a concern**
for this design; startup latency is the sole objective. That simplification is
load-bearing for the compressed-texture decision.

### Mesh structure — why meshes ship whole

At the sokol seam, `sg_make_buffer` hands the engine only: byte size, buffer type
(vertex/index/storage), `usage`, and the raw bytes. It does **not** carry the
vertex layout (stride/attribute formats — those live in the *pipeline*,
`sg_pipeline_desc.layout`), the index width (`index_type`), or the vertex count.
Full structure is *reconstructable by correlating* `make_buffer` + `make_pipeline`
+ `apply_bindings` + `draw`, but it is not handed over, and ge's own
`MeshVertex`/`Model` semantics live *above* the seam. Therefore generic mesh
decimation is not merely out of scope — **reconstructable ≠ safely-decimatable**
(UV/normal coherence, ordering); the failure mode is visible cracks. Meshes ship
whole.

Meshes are also the small side of the ledger: texture cost scales with **area**
(× maps for PBR); mesh cost with **vertex count**. A 1024² RGBA texture is 4 MB; a
mesh using it is tens-to-low-hundreds of KB. For ge (quads, a globe, a buggy
chassis) the gap is extreme. Both arguments point the same way.

### The giant-mesh boundary (non-goal)

A game shipping dense meshes (CAD / photogrammetry / micropolygon) would gate
first paint on geometry that is neither small nor safely decimatable at the seam.
The only fixes are app-provided mesh LODs (breaks invisibility) or eating the
latency. Out of scope; ge has no such content.

## Asset taxonomy and recipe verbs

ge's *largest* textures are mostly rasterised vector and text
(`ge::rasterizeSvg` / `rasterizeText` bake lunasvg/FreeType output into an
`sg_image`). ge knows their *source recipe*, so the cheapest delivery is to **ship
the recipe and rasterise on the player** — KB on the wire *and* native-resolution
crispness (the original motivation for considering draw-call streaming at all).
Three tiers under "minimise startup bytes":

1. **Meshes / geometry** — whole, on B. Small.
2. **Recipe-known textures (SVG, text, encoded images)** — ship the recipe,
   rasterise/decode on the player. Most of ge's texture footprint; collapses the
   dominant wire cost to KB and delivers native-res quality. Bypasses the mip
   machinery entirely.
3. **Opaque / procedural textures (no retained source)** — coarse-mip-first
   (above); the dynamic/churning ones stay on video.

Tier 2 needs an above-the-seam hook: "this `sg_make_image` came from
`rasterizeSvg`" is invisible to a generic sokol backend. In remoting mode ge's
rasterise/load functions register the *recipe* as the asset instead of
rasterising-then-uploading.

**Verb roadmap (fast-follows, not MVP):**

- **SVG** — especially the *interactive* `Document` case: ship the document once,
  then a stream of mutations (`applyStyleSheet`, `setAttribute`); the player
  re-rasterises at native res. Beats re-uploading a texture per state change.
- **Text** — string + font ref + size + colour → bytes, native-res. Pulls in
  **fonts as a cacheable asset**.
- **Image decode** — ship the encoded PNG/JPEG, decode on the player (size win,
  no resolution win).

`Sprite`/`SpriteBatch` (thin textured quads), the debug overlay, and any raw-`sg_*`
custom game rendering flatten to sokol — no verb.

**Day-one constraint on the spike:** design the asset-registration boundary to
carry a *recipe*, not just bytes, even though MVP ships no verbs — so verbs slot
in later without reshaping the asset path.

## Compressed textures — declined for MVP, probably indefinitely

Block-compressed formats (BCn/ASTC/ETC2) genuinely complicate delayed-LOD: 4×4
block granularity, degenerate sub-block mips, block-aligned partial updates,
per-backend transcode. **But that complexity exists in service of VRAM and
on-device upload efficiency** — which this design has explicitly deprioritised
(memory is not a concern; startup latency is). Coarse-mip-first nails startup
latency *regardless of format*. Decision tree:

1. **MVP-aligned: raw RGBA8 mips, coarse-first.** Universal on every sokol
   backend, no transcode, no block alignment, no degenerate-mip handling, no
   per-backend format matrix. The proven *policy* (below) applies directly; the
   block-format complexity simply doesn't exist. **Simpler than the Dawn-era
   pipeline**, which was paying for VRAM we no longer care about.
2. **If wire bytes ever bite: zstd the raw mip tails** — general-purpose,
   format-agnostic, no GPU-format coupling.
3. **Only if VRAM ever matters (it doesn't today): GPU block compression.** Then a
   genuinely new decision, because the sokol backend matrix (BC on desktop/Win/mac,
   ASTC on iOS/modern-Android, ETC2 baseline) is broader than the Dawn-era ASTC+ETC2
   pair — which points at **KTX2 / Basis Universal** (one universal source →
   transcodes to any GPU format, native progressive mips) over reviving the bespoke
   container. The astc-encoder and etcpak vendoring are still in-tree if we go
   bespoke instead.

## Caching — two caches, two purposes, one namespace

| Cache | Purpose | Key | Scope |
|---|---|---|---|
| **Player** | transmission dedup (save bandwidth) — "never receive the same bytes twice" | content hash of the delivered artifact | per-device, persistent on disk |
| **Server** | computation memoisation (save CPU) — "never compute the same derivation twice" | (source identity + derivation params) → output | per-server, amortised across players |

They are orthogonal (bandwidth vs CPU) and compose through **content-addressing**:
the *output content-hash* is simultaneously the server's stored-artifact identity
and the player's dedup/probe key. The server additionally keeps an *input-side
derivation key* (source + target format + mip + encoder settings + coarse-cap) to
answer "have I already computed this?" without recomputing.

**Consequence — work migrates server-side.** The Dawn era transcoded ASTC→ETC2
*on device*. The server already knows the player's backend (advertised), so it
derives the right format directly, computes it **once per (asset, format)**, and
serves every player of that backend class. Expensive derivations go from
O(players × assets × formats) to O(assets × formats). The player cache then shrinks
to pure final-artifact transmission-dedup.

**For static assets, the server "cache" is the offline bake we already own**
(`TextureEncoder` → `.getex`): pre-derive mip chains + per-backend variants at
asset-bake time and the server cache is *warm at boot*, zero runtime derivation.
Runtime server caching only earns its keep for what can't be pre-baked: procedural
textures and **verb-fallback rasterisations** (server rasterising an SVG/text verb
for a too-old player) — keyed by (recipe + params), amortised across sessions.

**Cold-server first-connect risk + mitigations.** Content-addressing the player
probe requires the server to *know the output hash*, i.e. to derive before probing —
so a cold server makes the *first* connection pay derivation on the critical
(startup) path. Mitigated, and the mitigations fall out of decisions already made:
(a) pre-baked static assets ⇒ cache warm at boot; (b) coarse mips are cheap to
derive inline on B, while the expensive compression is deferred to C and cached
for later players. Cost ordering and LOD structure reinforce each other:
cheap-coarse-inline, expensive-derivation-deferred-and-amortised.

**Free hygiene:** both caches are content-addressed immutable stores, so **neither
needs invalidation — only GC.** A changed source or bumped encoder version changes
the key and lands as a new entry; stale outputs are never served, they age out.

Existing target 🎯T3 (player sends a *preemptive* mip-cache manifest before
rendering) is the optimisation that layers on top of the baseline player cache —
reframed from the Dawn-wire model to the sokol command stream.

## Recovered prior art — the Dawn phase (Feb–Mar 2026)

This was built once, and far, then deleted in the bgfx migration — **except the
offline encoder and both compressors, which still build today.** The commit
messages are the rich record (mnemo predates good coverage here).

**What existed (read at peak `git show 5852849:<path>`):**

- **`.getex`/`.sqtex` container** — self-describing mip chain (magic-detected),
  ASTC 4×4 / ETC2 EAC RGBA8 / PNG-per-level. (`include/ge/GeTexFormat.h`,
  `src/TextureEncoder.cpp` — **still in tree**, deliberately outside `libge.a`.)
- **Compressors** — ARM astc-encoder (live submodule) + wolfpld/etcpak (vendored
  source) — **both still in tree**. No KTX2/Basis/zstd; bespoke container.
- **Runtime ASTC→ETC2 transcode + on-device cache** (`src/Texture.cpp`,
  `transcodeAstcToEtc2` / `transcodeCachePath`), including per-mip 4×4
  block-alignment padding for degenerate small mips. (`0c4d1e5`)
- **Deferred-LOD wire streamer** (`src/WireSession.cpp`,
  `SocketSerializer::Flush`): WriteTexture payloads over a 4 KB threshold dropped
  from the frame, the matching view's `baseMipLevel` bumped so the first frame
  paints from coarse mips; deferred mips queued **smallest-tail-first** (all
  textures sharpen together), streamed one-per-idle with a yield per mip (visibly
  sharpens, no snap). (`5c86c7d`→`65271d8`→`34524f7`→`d0db5d6`→`11795fd`→`594dfbf`)
- **Hash-probe cross-session mip cache** (`165ce28`): split each mip
  head(128 B IDs)/tail(pixels); server sends `head + fnv1a64(tail)` probe; player
  answers HIT (skip) / MISS (resend). **Measured 74 MB saved on warm connect.**

**Reusability:**

- *Lifts as-is (in tree):* astc-encoder + etcpak vendoring; `TextureEncoder.cpp` +
  `GeTexFormat.h` (renderer-agnostic container + box-filter mip-gen).
- *Lifts as algorithm (re-code for sokol):* transcode-on-load + content-addressed
  cache (pure CPU/fs; only the capability check changes); the hash-probe protocol;
  smallest-first priority queue + paced one-per-idle + yield-per-step; the
  coarse-first + view-rebind policy.
- *Dies (Dawn-specific):* the wire-command interception (hardcoded Dawn opcode/byte
  offsets) and **`MipTracker`'s view/bind-group fabrication + `SetBindGroup`
  rewriting** — the hardest, ugliest part — existed **only** because Dawn's wire
  model decouples upload from bind-group creation behind opaque ObjectIds. In a
  design where *we own the command vocabulary*, that problem dissolves: stream a
  coarse `sg_image`, then send a "replace/update image" verb the player applies in
  place. The worst part is the part we don't rebuild.

**Era boundaries:** start `785f7ea` (2026-02-03 "Migrate from bgfx to Dawn"),
removal `76bd346`→`0b65688`→`bf97631` (2026-03-23..04-12). Peak: `5852849`.

## The spike (🎯T128.2)

Bind `g_ge_sg_api` to a serialising null backend on the server; write a minimal
replay loop on the player; both behind a session-negotiated capability, so the
shipping video path is untouched (the dispatch shim isolates it, and the relay is
already agnostic — no spyder change needed). Prove a tiltbuggy frame round-trips,
then measure the things only measurement can settle:

- **Headline metric: time-to-first-coarse-frame.**
- **Bandwidth vs the current H.264 stream** on tiltbuggy (diagnostic spin +
  parallax + button churn) and esfera — find where on the crossover real content
  lands.
- **Reconnect cost, warm vs cold asset cache.**
- **Flow control under a janky/slow player** — does the B/C split hold, or does
  head-of-line blocking on an asset upload stall the draw stream?

Go/no-go on the implementation fan-out (T128.3–T128.6, T128.9) is recorded here.

## Scope & non-goals

- **In:** an end-to-end-negotiated protocol ladder (H.264 baseline retained + a
  command-stream rung); MVP = pure sokol remoting, zero verbs, raw-RGBA mip-first
  delivery, content-addressed player + server caches; the broker (spyder) stays a
  protocol-agnostic byte relay.
- **Fast-follow:** SVG / text / image-decode recipe verbs (each with sokol-flatten
  fallback).
- **Out / non-goals:** removing or replacing the H.264 path (it is the permanent
  baseline); making the broker protocol-aware (spyder relays bytes, full stop);
  mesh LOD / progressive mesh; giant-mesh content; GPU block-compressed textures
  (deferred until VRAM is a felt constraint); VRAM residency management (memory is
  not a concern); a production-server-cost story (streaming is dev-gated per
  🎯T145); wire-mode parallax beyond what the player's local view transform gives
  for free.
- **Not a state-streaming design.** An adjacent option — stream *game state*,
  render on the player — was considered and set aside: it is smaller on the wire
  but requires the game's render code on the player, breaking app-agnosticism.
  This design keeps game logic on the server and moves only rasterisation.

## Open decisions

1. **Transport for B/C** — separate QUIC/pigeon streams vs one WebSocket with
   app-level interleave (the latter needs chunked C-yields-to-B).
2. **Coarse-cap value** — the size/byte budget that splits B-coarse from
   C-refinement (generous default; behaves as all-upfront below it).
3. **Epoch-barrier semantics** — confirmed gate on B-coarse, never on C.
4. **First verb after MVP** — interactive SVG document (highest value, exercises
   recipe + mutation-stream + native-res re-rasterise end-to-end) vs image-decode
   (trivial warm-up).
5. **Handshake shape** — how the capability advertisement is framed in-band so an
   agnostic relay passes it through untouched, and how the transport-rung and
   verb-set negotiations nest.

## Relation to existing targets

- **🎯T107 / T109** — the dispatch shim and its interposable-capture
  generalisation; this transport is the production capture layer.
- **🎯T145** — removal of the ged daemon; the broker role moves into **spyder**, a
  protocol-agnostic byte relay, and the streaming path is dev-only in release.
  Because spyder is agnostic, the command-stream rung needs no broker work and
  rides the same byte pipe as video; this design composes with T145 but does not
  block on it.
- **🎯T11** — pigeon transport; the command stream rides whatever byte pipe ge
  surfaces (ged → spyder → pigeon). Negotiation is end-to-end and compatible with
  an E2E-encrypted relay.
- **🎯T3** — preemptive mip-cache manifest; layers on the baseline player cache.
- **🎯T34** — player as a regular ge app; the replay player is one.
- **🎯T1** — Wasm server in player; in-process command routing is the same stream
  with no socket.
- **🎯T124 / T92** — `renderToPng` and the app-channel are also "serialise a ge
  frame," aimed at a PNG / an agent rather than a player; this transport sits at
  the same altitude, not in the sokol basement.

## Target subgraph

```
T128 (umbrella) ── converges on ──▶ T128.3, T128.4, T128.5, T128.6, T128.9
  T128.1  design note (this file)                     [contract-first]
  T128.2  spike + go/no-go              ← T128.1      [spike-then-decide]
  T128.3  transport priority classes    ← T128.2
  T128.4  resource delivery (mip-first) ← T128.2
  T128.5  player + server caching       ← T128.3      (blocks T3)
  T128.6  per-session no-encode runtime ← T128.3, T128.9
  T128.7  recipe verbs (fast-follow)    ← T128.4, T128.5
  T128.8  block-compressed LOD (deferred / set aside) ← T128.4
  T128.9  negotiated protocol ladder; spyder byte-agnostic ← T128.2
```

## References

- Historical: `docs/ge-remote.md` (the original Dawn-wire draw-call player).
- Dawn-era commits: format `77f9a21`; ASTC `8202b68`; ETC2 `f39d20a`;
  transcoder+cache `0c4d1e5`; deferral chain
  `5c86c7d`→`65271d8`→`34524f7`→`d0db5d6`→`165ce28`→`11795fd`→`594dfbf`; fixes
  `5139979`, `ce89a21`, `9054c8a`, `5852849`; removal `76bd346`, `0b65688`,
  `bf97631`.
- Still in tree: `src/TextureEncoder.cpp`, `include/ge/GeTexFormat.h`,
  `vendor/github.com/ARM-software/astc-encoder`, `vendor/github.com/wolfpld/etcpak`.
```
