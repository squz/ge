# T158/T159 live smoke — Jevons, 2026-07-18 23:10 (+10:00)

- SP2A: server logged "SP2A arm=0 → primary seat" on attach (real-accel seat, synth-less, never armed); glass logged receipt.
- SP2F: PresentTrace on-glass: emitLat avg 52-64 ms / max 67 ms over Wi-Fi (pacing queue 2-4 frames; cross-clock, informative).
- Loopback oracle (same clock): emit→receipt median 0.11-0.15 ms vs 150 ms documented gate; arm transitions [0,1,0] observed in both gesture scenarios; never-armed verified for the real-accel seat.
- Oracle evidence: verdicts/T156.6/20260718T130828Z (all scenarios PASS, incl. new armstate/emit-latency checks).
