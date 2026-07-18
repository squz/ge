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
