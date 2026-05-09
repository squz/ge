// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/SpriteBatch.h>

#include <ge/FileIO.h>
#include <ge/Resource.h>

#include <bgfx/bgfx.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace ge {

namespace {

struct State {
    bgfx::ProgramHandle program     = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler     = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout  layout;
    bool                layoutReady = false;
};

constexpr uint64_t kDefaultBlend =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
    BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);

bgfx::ShaderHandle loadShader(const std::string& path) {
    auto stream = openFile(path, /*binary=*/true);
    if (!stream || !*stream) return BGFX_INVALID_HANDLE;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(*stream)),
                              std::istreambuf_iterator<char>());
    if (data.empty()) return BGFX_INVALID_HANDLE;
    const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(data.size() + 1));
    std::memcpy(mem->data, data.data(), data.size());
    mem->data[data.size()] = '\0';
    return bgfx::createShader(mem);
}

// Global lazy-init state (same pattern as SvgSprite.cpp).
State& globalState() {
    static State s;
    return s;
}

} // namespace

bool SpriteBatch::ensureState() {
    auto& s = globalState();
    if (bgfx::isValid(s.program)) return true;

    const std::string shaderDir = renderShaderDir();
    bgfx::ShaderHandle vs = loadShader(resource(shaderDir + "/ge_sprite_vs.bin"));
    bgfx::ShaderHandle fs = loadShader(resource(shaderDir + "/ge_sprite_fs.bin"));
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        spdlog::error("SpriteBatch: failed to load sprite shaders from {}", shaderDir);
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return false;
    }
    s.program = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
    if (!bgfx::isValid(s.sampler)) {
        s.sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    }
    if (!s.layoutReady) {
        s.layout.begin()
            .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8,
                 /*normalized=*/true)
            .end();
        s.layoutReady = true;
    }
    return true;
}

SpriteBatch::SpriteBatch() : blendState_(kDefaultBlend) {}
SpriteBatch::~SpriteBatch() = default;

void SpriteBatch::clear() {
    sprites_.clear();
    blendState_ = kDefaultBlend;
}

void SpriteBatch::setBlendState(uint64_t state) {
    blendState_ = state;
}

void SpriteBatch::addSprite(float cx, float cy, float hw, float hh,
                            bgfx::TextureHandle tex,
                            float uvL, float uvT, float uvR, float uvB,
                            uint32_t color) {
    if (!bgfx::isValid(tex)) return;
    Sprite s;
    s.tex = tex;
    // Two CCW triangles: bl, br, tr, bl, tr, tl
    s.verts[0] = {cx - hw, cy - hh, 0.f, uvL, uvB, color};  // bl
    s.verts[1] = {cx + hw, cy - hh, 0.f, uvR, uvB, color};  // br
    s.verts[2] = {cx + hw, cy + hh, 0.f, uvR, uvT, color};  // tr
    s.verts[3] = {cx - hw, cy - hh, 0.f, uvL, uvB, color};  // bl
    s.verts[4] = {cx + hw, cy + hh, 0.f, uvR, uvT, color};  // tr
    s.verts[5] = {cx - hw, cy + hh, 0.f, uvL, uvT, color};  // tl
    sprites_.push_back(s);
}

void SpriteBatch::addQuad(const Rect& dest, bgfx::TextureHandle tex,
                          const Rect& uvs, uint32_t color) {
    if (!bgfx::isValid(tex)) return;
    const float cx  = dest.x + dest.w * 0.5f;
    const float cy  = dest.y + dest.h * 0.5f;
    const float hw  = dest.w * 0.5f;
    const float hh  = dest.h * 0.5f;
    const float uvL = uvs.x;
    const float uvT = uvs.y;
    const float uvR = uvs.x + uvs.w;
    const float uvB = uvs.y + uvs.h;
    addSprite(cx, cy, hw, hh, tex, uvL, uvT, uvR, uvB, color);
}

void SpriteBatch::submit(bgfx::ViewId view) {
    if (sprites_.empty()) return;
    if (!ensureState()) return;

    auto& s = globalState();

    // Group consecutive sprites sharing the same texture and flush each run.
    // This preserves insertion order while minimising state changes.
    std::size_t i = 0;
    while (i < sprites_.size()) {
        bgfx::TextureHandle curTex = sprites_[i].tex;
        std::size_t j = i + 1;
        while (j < sprites_.size() && sprites_[j].tex.idx == curTex.idx) {
            ++j;
        }
        const std::size_t count = j - i;

        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb,
            static_cast<uint32_t>(count * 6), s.layout);
        for (std::size_t k = 0; k < count; ++k) {
            std::memcpy(tvb.data + k * 6 * sizeof(SpriteVertex),
                        sprites_[i + k].verts,
                        6 * sizeof(SpriteVertex));
        }

        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setTexture(0, s.sampler, curTex);
        bgfx::setState(blendState_);
        bgfx::submit(view, s.program);

        i = j;
    }

    blendState_ = kDefaultBlend;
}

} // namespace ge
