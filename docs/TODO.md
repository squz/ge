# TODO

- Games should hide the iOS status bar at the top by default.

- **A9 sensor-event flood** (2026-07-18): a flat-on-table SM-X110 emitted a
  burst of 175 bit-identical accelerometer events ([-9.41,+0.04,+2.78], a
  73° gravity vector) in one pump — a stale/corrupt event replayed in the
  Android sensor→SDL→forward path, possibly on screen-dim/lock. Reproduce
  with the player attached and the tablet idling to screen-off; instrument
  PlayerRender's sensor forwarding (count + dedup window). 🎯T161's
  identical-sample-flood detector formalizes the invariant.
