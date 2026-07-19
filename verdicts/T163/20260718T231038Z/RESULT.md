# 🎯T163 evidence — per-session game instances

## Mechanical oracle (this dir)
scripts/t163-oracle PASS: two headless players attached concurrently with
opposite scripted sensor streams; the mother spawned two child processes
(mother.log), each child's game-boundary trace shows its own glass's
sensors only (dominant gx −3 vs +3 — mirrored, independent worlds), and
the survivor's world ran on for 3.5 s after the other detached.

## Live proof (screenshots)
live-jevons.png / live-pixel.png: Jevons and Pixel attached simultaneously
to the same catalogue name; each received its own child instance
(sessions s24/s25, mother log) and shows its own buggy at its own
position under its own tilt.

## Regressions
342/342 unit tests; scripts/t156-oracle PASS end-to-end through the
mother→child path (single-session behaviour unchanged per instance).
