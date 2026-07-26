// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.2 Runtime tile-pyramid loading: open a .getp pack, page every tile
// into GPU memory (background reads + render-thread uploads), then render
// fully resident — no steady-state streaming.
//
// Threading contract: open() and pump() are render-thread calls. A reader
// thread owns file I/O and hands decoded blobs to pump(), which creates at
// most `budget` immutable sg_images per call (default from tweak
// "tilepack.uploads_per_frame"). All tile queries are render-thread-only.
//
// Load order is coarse level → fine, so level 0 of every plane is
// renderable after the first pump or two (~ms), and resolve() serves any
// tile from its deepest resident ancestor until the leaf arrives.

#pragma once

#include <ge/CubeSphere.h>
#include <ge/TilePackFormat.h>

#include "sokol_gfx.h"

#include <memory>
#include <string>

namespace ge {

struct TilePyramidOptions {
    // Deepest pyramid level to load per plane (-1 → all levels in the pack).
    // DeviceTier.h maps the device tier to this cap.
    int levelCap = -1;
    // Upload budget per pump() call; <=0 → tweak "tilepack.uploads_per_frame".
    int uploadsPerPump = 0;
};

// One resident tile. view samples the stored (guttered) texture; use
// ge::tilePayloadToTexUV (or bake the transform into patch UVs) to address
// payload texels.
struct TileView {
    sg_view  view = {};
    // Level actually resolved (== requested level once fully resident;
    // an ancestor while loading). uvScale/uvBias remap the requested tile's
    // payload UVs into this tile's payload UVs.
    uint8_t    level   = 0;
    la::float2 uvScale = {1.0f, 1.0f};
    la::float2 uvBias  = {0.0f, 0.0f};
    bool valid() const { return view.id != SG_INVALID_ID; }
};

class TilePyramid {
public:
    TilePyramid();
    ~TilePyramid();
    TilePyramid(TilePyramid&&) noexcept;
    TilePyramid& operator=(TilePyramid&&) noexcept;
    TilePyramid(const TilePyramid&)            = delete;
    TilePyramid& operator=(const TilePyramid&) = delete;

    // Opens the pack, reads header + indices, starts the reader thread.
    // Returns a null pyramid (isNull()) on malformed/unreadable packs; logs.
    static TilePyramid open(const std::string& path,
                            const TilePyramidOptions& opts = {});

    bool isNull() const;

    // Render-thread: drain decoded blobs into sg_images. Cheap when idle.
    void pump();

    // Plane index by desc name ("rgb", "id", "sdf"); -1 if absent.
    int planeIndex(const char* name) const;
    const GeTilePlaneDesc& planeDesc(int plane) const;

    // True once every plane's level 0 is resident (renderable globe).
    bool baseResident() const;
    // True once everything (up to levelCap) is resident.
    bool fullyResident() const;
    // Bytes uploaded so far / total to upload — progress reporting.
    uint64_t bytesResident() const;
    uint64_t bytesTotal() const;

    // Deepest-resident view for a tile, walking up ancestors while loading.
    // Invalid only before baseResident().
    TileView resolve(int plane, CubeFace face, uint8_t level,
                     uint16_t tx, uint16_t ty) const;

private:
    struct M;
    std::unique_ptr<M> m;
};

} // namespace ge
