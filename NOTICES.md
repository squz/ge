# Third-Party Notices

ge bundles and links against the following third-party libraries.
This file provides the required attribution under each licence.

---

## Licensing gaps and warnings

### Triangle (J. R. Shewchuk)

Triangle has a **non-standard, restrictive licence** — see the full
terms in `vendor/src/triangle.c`. Key restrictions:

- Free for private, research, and institutional use.
- Redistribution requires retaining the copyright header unchanged.
- Modifications must preserve the original copyright, source and object
  code must be freely available, and changes must be clearly noted.
- **Commercial distribution is only permissible by direct arrangement
  with Jonathan Shewchuk (jrs@cs.berkeley.edu)**, with one carve-out:
  if you do not directly supply Triangle to your customer but instead
  tell them where to obtain it for free (e.g. by referring them to
  ge's public GitHub repository), no arrangement is required.

**How this affects ge consumers**

Triangle is **not linked into `libge.a`**. Nothing under `src/` calls
`triangulate()` or `#include`s `triangle.h`. The file `vendor/src/triangle.c`
is compiled only when a consumer explicitly references the
opt-in `ge/TRIANGLE_OBJ` make variable in their link line (see
`Module.mk`). Downloading ge from GitHub does not itself constitute
commercial redistribution of Triangle, so open-source / research /
institutional consumers are unaffected.

**If you are building a commercial product**, you have three options:

1. **Do nothing** and leave `ge/TRIANGLE_OBJ` out of your link line.
   ge itself will build and link cleanly; no Triangle code will be
   compiled into your binary. This is the safe default.
2. **Replace with earcut.hpp** (also vendored, ISC-licensed, header-only
   at `vendor/include/earcut.hpp`) for simple polygon triangulation.
   earcut.hpp does not do constrained Delaunay or quality refinement,
   so it is not a drop-in substitute for every Triangle use-case.
3. **Contact Jonathan Shewchuk** to arrange commercial licensing if
   your build genuinely needs Triangle's constrained Delaunay /
   quality-mesh features and you intend to ship them in a paid product.

### FFmpeg (prebuilt, Android only)

FFmpeg is vendored as prebuilt static libraries under
`vendor/ffmpeg/lib/android-arm64/` (libavcodec, libavutil, libswscale)
and used only in `src/bridge/VideoDecoder_ffmpeg.cpp`, which is
compiled solely for Android. The build was configured **without GPL
components** (`CONFIG_GPL 0` in `vendor/ffmpeg/include/config.h`), so
**LGPL-2.1** applies. Because ge links FFmpeg statically and LGPL
requires the user to be able to re-link against a modified FFmpeg,
either:

1. Provide build instructions so users can relink (satisfies LGPL
   §6(d)), or
2. Switch to dynamic linking for the FFmpeg libraries.

The submodule source at `vendor/github.com/FFmpeg/FFmpeg` is present
for reference only; only the prebuilt libs under `vendor/ffmpeg/` are
compiled into the Android player.

---

## Submoduled libraries (vendor/github.com/)

### astc-encoder
- Source: https://github.com/ARM-software/astc-encoder
- Version: commit `701503966b` (tag `5.3.0-10-g7015039`)
- Licence: Apache-2.0
- Copyright: © 2011–2024 Arm Limited. All rights reserved.
- Notice: Licensed under the Apache License, Version 2.0. See
  `vendor/github.com/ARM-software/astc-encoder/LICENSE.txt`.

### sokol (sokol_gfx)
- Source: https://github.com/floooh/sokol
- Licence: zlib/libpng
- Copyright: © 2018 Andre Weissflog.
- Notice: ge's rendering backend (🎯T38). Compiled into `libge.a` /
  `libgesokol-{gles,vk}.so` from `vendor/github.com/floooh/sokol/sokol_gfx.h`.
  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to: (1) the origin is not misrepresented, (2) altered
  source versions are clearly marked, and (3) the notice is not removed.
  See the licence block at the foot of `sokol_gfx.h`. **ge carries a local
  patch** to `_sg_vk_init_caps` (🎯T107: clamp unlimited descriptor limits
  before the `int` cast); altered-source-marked per clause (2).

### sokol-tools-bin (sokol-shdc)
- Source: https://github.com/floooh/sokol-tools-bin
- Licence: MIT
- Copyright: © Andre Weissflog and sokol-tools contributors.
- Notice: Build-time shader compiler (`sokol-shdc`); not redistributed in
  shipped binaries. See `vendor/github.com/floooh/sokol-tools-bin/LICENSE`.

### asio
- Source: https://github.com/chriskohlhoff/asio
- Version: commit `55684d42ac` (tag `asio-1-36-0`)
- Licence: Boost Software Licence 1.0 (BSL-1.0)
- Copyright: © 2003–2025 Christopher M. Kohlhoff.
- Notice: Distributed under the Boost Software License, Version 1.0.
  See `vendor/github.com/chriskohlhoff/asio/COPYING` or
  http://www.boost.org/LICENSE_1_0.txt.

### box2d
- Source: https://github.com/erincatto/box2d
- Version: commit `8c661469c9` (tag `v3.1.1`)
- Licence: MIT
- Copyright: © 2022 Erin Catto.
- Notice: Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software, to deal in the Software without
  restriction, subject to the conditions that the above copyright
  notice and this permission notice are included in all copies or
  substantial portions. See `vendor/github.com/erincatto/box2d/LICENSE`.

### spdlog
- Source: https://github.com/gabime/spdlog
- Version: commit `472945ba48` (based on tag `v1.2.1`)
- Licence: MIT
- Copyright: © 2016–present Gabi Melman and spdlog contributors.
- Notice: See `vendor/github.com/gabime/spdlog/LICENSE`.

### SDL (SDL3)
- Source: https://github.com/libsdl-org/SDL
- Version: commit `a962f40bbb` (tag `release-3.4.0`)
- Licence: zlib
- Copyright: © 1997–2025 Sam Lantinga.
- Notice: Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to: (1) the origin is not
  misrepresented, (2) altered source versions are clearly marked, and
  (3) the notice is not removed from source distributions. See
  `vendor/github.com/libsdl-org/SDL/LICENSE.txt`.

### SDL_image (SDL3_image)
- Source: https://github.com/libsdl-org/SDL_image
- Version: 3.4.0 (vendored in-tree; not a git submodule)
- Licence: zlib
- Copyright: © 1997–2026 Sam Lantinga.
- Notice: Same terms as SDL above. See
  `vendor/github.com/libsdl-org/SDL_image/LICENSE.txt`.

### SDL_ttf (SDL3_ttf)
- Source: https://github.com/libsdl-org/SDL_ttf
- Version: commit `a1ce367` (tag `release-3.2.2`)
- Licence: zlib
- Copyright: © 1997–2025 Sam Lantinga.
- Notice: Same terms as SDL above. See
  `vendor/github.com/libsdl-org/SDL_ttf/LICENSE.txt`. Built from
  source on Android and shipped as prebuilt xcframework on Apple
  platforms; both paths pull in freetype, harfbuzz, plutosvg, and
  plutovg (attributed below).

### FreeType (via SDL_ttf)
- Source: https://github.com/freetype/freetype
- Version: commit `9973564cf` (based on tag `VER-2-13-2`)
- Licence: FreeType Licence (FTL, BSD-style) OR GPL-2.0 — licensee's
  choice; ge uses under FTL. Source tree contains both texts.
- Copyright: © 1996–2024 David Turner, Robert Wilhelm, Werner Lemberg
  and the FreeType Project.
- Notice: Portions of this software are copyright © The FreeType
  Project (www.freetype.org). All rights reserved. See
  `vendor/github.com/libsdl-org/SDL_ttf/external/freetype/LICENSE.TXT`
  and `docs/FTL.TXT`.

### HarfBuzz (via SDL_ttf)
- Source: https://github.com/harfbuzz/harfbuzz
- Version: commit `564bf9818` (based on tag `8.5.0`)
- Licence: "Old MIT" (MIT-style)
- Copyright: © 2010–2022 Google, Inc. and contributors.
- Notice: See `vendor/github.com/libsdl-org/SDL_ttf/external/harfbuzz/COPYING`.

### plutosvg (via SDL_ttf)
- Source: https://github.com/sammycage/plutosvg
- Version: commit `2983eb6` (based on tag `v0.0.6`)
- Licence: MIT
- Copyright: © 2020–2025 Samuel Ugochukwu.
- Notice: See `vendor/github.com/libsdl-org/SDL_ttf/external/plutosvg/LICENSE`.

### plutovg (via SDL_ttf)
- Source: https://github.com/sammycage/plutovg
- Version: commit `3e6f922` (tag `v0.0.12`)
- Licence: MIT
- Copyright: © 2020–2024 Samuel Ugochukwu.
- Notice: See `vendor/github.com/libsdl-org/SDL_ttf/external/plutovg/LICENSE`.

### lunasvg
- Source: https://github.com/sammycage/lunasvg
- Version: tag `v3.5.0` (commit `83c58df`)
- Licence: MIT
- Copyright: © 2020–2025 Samuel Ugochukwu.
- Notice: See `vendor/github.com/sammycage/lunasvg/LICENSE`.
- Used by: `ge::rasterizeSvg` — the canonical SVG rasterizer in
  ge. Compiled directly into `libge.a`; not used at runtime by SDL_ttf.

### plutovg (vendored under lunasvg, used by ge::rasterizeSvg)
- Source: https://github.com/sammycage/plutovg (shipped as a nested
  subdirectory of lunasvg v3.5.0 — `vendor/github.com/sammycage/lunasvg/plutovg/`)
- Version: 1.3.1 (the version lunasvg v3.5.0 pins).
- Licence: MIT (with portions derived from FreeType under the FTL — see below).
- Copyright: © 2020–2025 Samuel Ugochukwu.
- Notice: See `vendor/github.com/sammycage/lunasvg/plutovg/LICENSE`.
- Distinct from the older plutovg `v0.0.12` shipped via SDL_ttf for
  color emoji support — see "plutovg (via SDL_ttf)" entry above. Both
  are present in the build: SDL_ttf's prebuilt path satisfies its own
  references; the lunasvg-bundled plutovg satisfies ge::rasterizeSvg.

### FreeType (portions, vendored under plutovg)
- Source: plutovg's `plutovg-ft-*.c` files derive raster / stroker /
  math code from FreeType. See
  `vendor/github.com/sammycage/lunasvg/plutovg/FTL.TXT`.
- Licence: FreeType Licence (FTL, BSD-style) OR GPL-2.0 — licensee's
  choice. ge consumes under FTL.
- Copyright: © 1996–2024 The FreeType Project (David Turner, Robert
  Wilhelm, and Werner Lemberg).
- Notice: The full FTL text is reproduced in
  `vendor/github.com/sammycage/lunasvg/plutovg/FTL.TXT`.

### stb_image / stb_image_write / stb_truetype (portions, vendored under plutovg)
- Source: plutovg's `plutovg-stb-*.h` files are vendored copies of
  Sean Barrett's stb single-file libraries.
- Licence: MIT OR Public Domain (dual; ge consumes under MIT).
- Copyright: © 2017 Sean Barrett.
- Notice: The dual-licence text is at the bottom of each
  `vendor/github.com/sammycage/lunasvg/plutovg/source/plutovg-stb-*.h`.

### QR-Code-generator
- Source: https://github.com/nayuki/QR-Code-generator
- Version: commit `2c9044de6b` (tag `v1.8.0`)
- Licence: MIT
- Copyright: © Project Nayuki. https://www.nayuki.io/
- Notice: Permission is hereby granted, free of charge, subject to the
  conditions that the above copyright notice and permission notice are
  included in all copies or substantial portions. See
  `vendor/github.com/nayuki/QR-Code-generator/c/qrcodegen.h`.

### liteparser
- Source: https://github.com/sqliteai/liteparser
- Version: vendored in-tree (not a git submodule)
- Licence: MIT
- Copyright: © 2026 SQLite AI.
- Notice: See `vendor/github.com/sqliteai/liteparser/LICENSE`.

### etcpak
- Source: https://github.com/wolfpld/etcpak
- Version: vendored in-tree (not a git submodule)
- Licence: BSD 3-Clause
- Copyright: © 2013–2025 Bartosz Taudul.
- Notice: Redistributions must retain the copyright notice and
  conditions. See `vendor/github.com/wolfpld/etcpak/LICENSE.txt`.

### FFmpeg (LGPL-2.1, Android only)
- Source: https://github.com/FFmpeg/FFmpeg
- Version: commit `56b97c03d4` (tag `n8.2-dev`)
- Licence: LGPL-2.1-or-later (compiled without GPL components)
- Copyright: © 2000–2024 the FFmpeg developers.
- Notice: See _Licensing gaps and warnings_ above for LGPL relinking
  obligations. Licence text: `vendor/github.com/FFmpeg/FFmpeg/COPYING.LGPLv2.1`.

---

## Single-header and amalgamation files (vendor/include/, vendor/src/)

### doctest.h
- Source: https://github.com/doctest/doctest
- Licence: MIT
- Copyright: © 2016–2023 Viktor Kirilov.
- Notice: Distributed under the MIT Software License. See header or
  https://opensource.org/licenses/MIT.

### earcut.hpp
- Source: https://github.com/mapbox/earcut.hpp
- Licence: ISC
- Copyright: © Mapbox contributors.
- Notice: The ISC licence permits use, copy, modification, and
  distribution subject to retaining the copyright notice. Upstream
  licence: https://github.com/mapbox/earcut.hpp/blob/master/LICENSE.

### linalg.h
- Source: https://github.com/sgorsten/linalg
- Licence: The Unlicense (public domain)
- Copyright: Sterling Orsten (original author). No copyright reserved.
- Notice: This is free and unencumbered software released into the
  public domain. See https://unlicense.org/.

### lz4.h / lz4.c
- Source: https://github.com/lz4/lz4
- Licence: BSD 2-Clause (library files)
- Copyright: © Yann Collet. All rights reserved.
- Notice: See `vendor/LICENSE.lz4` and the file headers.

### minimp3.h / minimp3_ex.h
- Source: https://github.com/lieff/minimp3
- Licence: CC0-1.0 (public domain dedication)
- Copyright: Dedicated to the public domain by the authors.
- Notice: To the extent possible under law, the authors have dedicated
  all copyright and related rights to the public domain worldwide. See
  https://creativecommons.org/publicdomain/zero/1.0/.

### nlohmann/json (json.hpp)
- Source: https://github.com/nlohmann/json
- Version: 3.11.3
- Licence: MIT
- Copyright: © 2013–2023 Niels Lohmann.
- Notice: See `vendor/include/nlohmann/json.hpp` header or
  https://github.com/nlohmann/json/blob/develop/LICENSE.MIT.

### sha1.h
- Source: Written in-house; based on RFC 3174.
- Licence: Public domain
- Copyright: None claimed. See file header.
- Notice: "Public domain — based on RFC 3174."

### sqlift.h / sqlift.cpp
- Source: https://github.com/squz/sqlift (internal)
- Licence: Apache-2.0
- Copyright: © 2026 Marcelo Cantos.
- Notice: Licensed under the Apache License, Version 2.0. See header.

### sqlpipe.h / sqlpipe.cpp
- Source: https://github.com/squz/sqlpipe (internal)
- Licence: Apache-2.0
- Copyright: © 2026 The sqlpipe Authors.
- Notice: Licensed under the Apache License, Version 2.0. See header.

### sqlite3.h / sqlite3.c
- Source: https://www.sqlite.org/
- Licence: Public domain
- Copyright: None claimed by the SQLite authors.
- Notice: "The author disclaims copyright to this source code." The
  SQLite amalgamation is in the public domain and may be used for any
  purpose.

### stb_image.h / stb_image_write.h
- Source: https://github.com/nothings/stb
- Licence: Public domain (alt: MIT)
- Copyright: Sean Barrett and contributors (public domain dedication).
- Notice: These files are dual-licenced public domain / MIT. No
  attribution is required, but including this notice fulfils the
  optional MIT alternative. See the end of each header for the full
  Unlicense / MIT notice.

### triangle.h / triangle.c
- Source: http://www.cs.cmu.edu/~quake/triangle.html
- Licence: **Non-standard — see warning at the top of this file.**
- Copyright: © 1996, 2005 Jonathan Richard Shewchuk.
- Notice: Free for non-commercial use. Commercial distribution requires
  direct arrangement with the author. See `vendor/src/triangle.c` for
  the full licence statement.
