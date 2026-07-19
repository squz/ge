# Fonts

Roboto Regular and Bold (googlefonts/roboto v2.138, unhinted), Apache
License 2.0 — see LICENSE-Roboto.

Named per ge's web font convention (`docs/web-platform.md` /
`src/FontLoader_web.cpp`): on the web build, `system:sans-serif[-bold]`
resolves to `fonts/sans-serif[-bold].ttf` in the preloaded FS — the browser
exposes no OS font files to wasm. Native platforms ignore this directory
and use real system fonts (macOS: Helvetica via CoreText; Android:
/system/fonts Roboto — the same face shipped here, so web matches Android).
