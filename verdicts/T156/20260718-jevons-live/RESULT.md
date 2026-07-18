# Live Jevons run — 2026-07-18 22:03 (+10:00)

- Player: Debug-iphoneos Player.app (capability build) via `launch_player`,
  pid 4757, STREAM_ADDR 192.168.1.217:3030.
- DeviceInfo: 2266x1488 @2x class=2 **caps=0x7**
  (cmdstream | dualSafe | **hasAccelerometer**).
- Server: `seat declares a real accelerometer — synth destroyed; the glass
  is the sensor authority` — no AccelSynth for this seat; wire sensor
  samples reach the game unfiltered.
- Transport: cmdstream (SP2S sprite runs), 60 fps sustained
  (frame#1200→#1260 in 1.000 s), 617 B/frame, no H.264.
- Gravity tracks the iPad's physical attitude continuously.
- 1–2 s delay root cause (recorded per T156.2): the server synth, armed by
  wire-forwarded finger/click events under GE_SERVER_BUILD, owned the
  sensor stream and suppressed the glass's real samples; gravity then
  followed stale synth tilt (touch-driven, ~80 ms ease after release,
  re-armed by every touch) instead of live accel — seconds-scale wrong
  gravity. Eliminated constructionally; loopback gesture→gravity now
  measured 5–57 ms (verdicts/T156.6/20260718T115558Z).

## Addendum — proof point confirmed (2026-07-18 ~22:55 +10:00)

The first live sessions exposed three glass-side defects the loopback
oracle cannot see (no display, no Wi-Fi): image uploads dropped with
skipped display frames (black glass), render-thread hitches from the
diagnostic log sink, and present-on-arrival judder from Wi-Fi burst
delivery. Fixed by the image side-channel, the queued log sink, and a
paced presentation FIFO (initial-only priming, cap 6). On-glass
PresentTrace telemetry: skips=0, holds>25ms=0, maxDt=17.2ms per 120
presents. Environmental fixes along the way: superseded servers now
exit on sideband close (ten orphaned 60 fps game loops were competing
for the host), and transport negotiation is idempotent (handshake
livelock with the promotion re-handshake).

**Human proof point: the user confirms tilt on Jevons is smooth AND
immediate over the sokol/SP2S path.** 🎯T156 achieved.
