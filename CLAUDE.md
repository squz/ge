# ge/ Engine Module

The canonical repository instructions live in [`AGENTS.md`](AGENTS.md) and are
imported below, so Claude Code reads them without a duplicated copy — edit
guidance in `AGENTS.md`, not this file.

The two machine-read directives are mirrored here as literal lines, because
tooling (the release skill's `discover.sh`, the gate / profile system) greps
`CLAUDE.md` for them and does not resolve the `@AGENTS.md` import. Keep them in
sync with AGENTS.md's copies:

homebrew_tap: disabled
profile: game

@AGENTS.md

## Plateau

**Plateau P (2026-07-11):** spyder is the sole dev control plane; `ged` is removed (🎯T145). Primary modality is direct `ge::run` + app-channel. Optional server-mode streams through spyder. See `AGENTS.md` § Architecture.
