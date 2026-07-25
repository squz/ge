// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T167 Public compressed-texture load surface.
//
// Cook (build-time) is Module.mk pattern rules + bin/ge-texenc.
// Runtime: call loadTexture / loadCube with a path stem (or concrete file);
// ge picks ASTC / ETC2 / PNG getex / raw PNG based on what the live sokol
// backend can sample and what files exist. Games never branch on encoding.

#pragma once

#include "sokol_gfx.h"

#include <functional>
#include <string>
#include <vector>

namespace ge {

// Owns an sg_image + sg_view (2D or cube). Move-only; destroy() is safe with
// no live sokol context (no-op when !sg_isvalid()).
struct GpuTexture {
    sg_image        image  = {};
    sg_view         view   = {};
    int             width  = 0;
    int             height = 0;
    int             numMips = 1;
    sg_image_type   type   = SG_IMAGETYPE_2D;
    sg_pixel_format format = SG_PIXELFORMAT_NONE;

    GpuTexture() = default;
    ~GpuTexture();
    GpuTexture(GpuTexture&&) noexcept;
    GpuTexture& operator=(GpuTexture&&) noexcept;
    GpuTexture(const GpuTexture&)            = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    bool isNull() const { return image.id == SG_INVALID_ID; }
    void destroy();
};

// Ordered candidate paths for a logical asset. `pathOrStem` may be:
//   "data/tex/foo.png"     — try that file, then stem siblings
//   "data/tex/foo"         — try foo.astc.getex, foo.etc2.getex, …
//   "data/tex/foo.astc.getex" — try that encoding first
//
// Pure: no I/O. hasAstc/hasEtc2 come from sg_query_pixelformat (or a test
// double). Prefer ASTC when sampleable, then ETC2, then .png.getex, then
// raw .png / .jpg.
std::vector<std::string> textureCandidatePaths(const std::string& pathOrStem,
                                               bool hasAstc, bool hasEtc2);

// First candidate for which `exists(path)` is true. Empty string if none.
// Pure aside from the exists callback (default: ge::resource + filesystem).
std::string resolveTexturePath(
    const std::string& pathOrStem, bool hasAstc, bool hasEtc2,
    const std::function<bool(const std::string&)>& exists);

// Convenience using default existence probe (ge::resource openable).
std::string resolveTexturePath(const std::string& pathOrStem);

// Load a 2D texture. Path may be a stem or concrete file (see
// textureCandidatePaths). Requires an initialized sokol context. On failure
// returns a null GpuTexture and logs.
GpuTexture loadTexture(const std::string& pathOrStem);

// Load a cube map from six faces named `facePrefix + "0".."5"` with the
// same extension resolution as loadTexture (e.g. facePrefix =
// "data/textures/cubemap/face_" → face_0.astc.getex … or face_0.png).
// All faces must resolve to the same encoding and equal dimensions.
// Requires sokol. On failure returns null + logs.
GpuTexture loadCube(const std::string& facePrefix);

// Whether the live backend can sample ASTC 4×4 / ETC2 RGBA8 (false if
// sokol is not set up).
bool textureBackendHasAstc();
bool textureBackendHasEtc2();

} // namespace ge
