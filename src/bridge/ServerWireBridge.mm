// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerWireBridge.h"

#include <ge/Protocol.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ge {

namespace {

// Lightweight write buffer for building wire messages.
struct WireWriter {
    std::vector<uint8_t> buf;
    explicit WireWriter(size_t cap = 0) { buf.reserve(cap); }
    template <typename T> void put(const T& v) {
        auto p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + sizeof(T));
    }
    void append(const void* d, size_t n) {
        auto p = static_cast<const uint8_t*>(d);
        buf.insert(buf.end(), p, p + n);
    }
    const uint8_t* data() const { return buf.data(); }
    size_t size() const { return buf.size(); }
};

} // namespace

struct ServerWireBridge::Impl {
    std::string id;
    std::shared_ptr<WsConnection> wire;
    int width = 0;
    int height = 0;
    bool dimensionsKnown = false;
    bool ready = false;

    // Session Context: built once dimensions arrive (in initialize),
    // refreshed each beginFrame. Brokered db is in-memory — persistence
    // is owned by the player and synced via sqlpipe.
    std::optional<Context> ctx;
    std::string schemaDdl;
    DeviceClass deviceClass = DeviceClass::Desktop;
    float pixelsPerPt = 1.0f;

    // Capture framebuffer + double-buffered readback.
    bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle renderTex = BGFX_INVALID_HANDLE;

    static constexpr int kNumReadback = 3;
    struct ReadbackSlot {
        bgfx::TextureHandle tex = BGFX_INVALID_HANDLE;
        std::vector<uint8_t> buf;
        uint32_t readyFrame = 0;
        bool pending = false;
    };
    ReadbackSlot readback[kNumReadback];
    int submitIdx = 0;

    std::unique_ptr<VideoEncoder> encoder;
    std::vector<uint8_t> pixelBuf;
    std::shared_ptr<uint32_t> encodeSeq = std::make_shared<uint32_t>(0);

    std::function<void(const SDL_Event&)> eventHandler;

    void submitCaptureBlit() {
        auto& slot = readback[submitIdx];
        if (slot.pending) return;
        bgfx::setViewFrameBuffer(255, BGFX_INVALID_HANDLE);
        bgfx::blit(255, slot.tex, 0, 0, renderTex);
        slot.readyFrame = bgfx::readTexture(slot.tex, slot.buf.data());
        slot.pending = true;
        submitIdx = (submitIdx + 1) % kNumReadback;
    }

    bool readCapturedFrame(uint32_t frameNum) {
        for (auto& slot : readback) {
            if (slot.pending && frameNum >= slot.readyFrame) {
                slot.pending = false;
                std::memcpy(pixelBuf.data(), slot.buf.data(), pixelBuf.size());
                return true;
            }
        }
        return false;
    }
};

ServerWireBridge::ServerWireBridge(std::string sessionId,
                                   std::shared_ptr<WsConnection> wire,
                                   const SessionHostConfig& config)
    : i_(std::make_unique<Impl>()) {
    i_->id = std::move(sessionId);
    i_->wire = std::move(wire);
    i_->schemaDdl = config.schemaDdl;
}

ServerWireBridge::~ServerWireBridge() {
    shutdown();
}

const std::string& ServerWireBridge::id() const { return i_->id; }
bgfx::FrameBufferHandle ServerWireBridge::framebuffer() const { return i_->fb; }

int ServerWireBridge::width() const  { return i_->width; }
int ServerWireBridge::height() const { return i_->height; }
DeviceClass ServerWireBridge::deviceClass() const { return i_->deviceClass; }
const Context& ServerWireBridge::context() const { return *i_->ctx; }

bool ServerWireBridge::hasDimensions() const { return i_->dimensionsKnown; }
bool ServerWireBridge::isReady() const { return i_->ready; }

bool ServerWireBridge::shouldQuit() const {
    return !i_->wire || !i_->wire->isOpen();
}

void ServerWireBridge::send(const wire::SessionConfig& cfg) {
    if (!i_->wire || !i_->wire->isOpen()) return;
    wire::MessageHeader hdr{};
    hdr.magic = wire::kSessionConfigMagic;
    hdr.length = sizeof(wire::SessionConfig);
    std::vector<uint8_t> msg(sizeof(hdr) + sizeof(cfg));
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    std::memcpy(msg.data() + sizeof(hdr), &cfg, sizeof(cfg));
    i_->wire->sendBinary(msg.data(), msg.size());
    SPDLOG_INFO("Session '{}': sent SessionConfig (sensors=0x{:x}, orientation={})",
                i_->id, cfg.sensors, cfg.orientation);
}

void ServerWireBridge::setEventHandler(std::function<void(const SDL_Event&)> h) {
    i_->eventHandler = std::move(h);
}

bool ServerWireBridge::pumpWire() {
    if (!i_->wire || !i_->wire->isOpen()) return false;

    while (i_->wire->isOpen() && i_->wire->available() > 0) {
        std::vector<char> data;
        if (!i_->wire->recvBinary(data)) break;
        if (data.size() < 8) continue;

        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), 4);

        if (magic == wire::kDeviceInfoMagic && !i_->dimensionsKnown &&
            data.size() >= sizeof(wire::MessageHeader) + sizeof(wire::DeviceInfo)) {
            wire::DeviceInfo info;
            std::memcpy(&info, data.data() + sizeof(wire::MessageHeader), sizeof(info));
            i_->width  = info.width / 2;
            i_->height = info.height / 2;
            if (i_->width == 0 || i_->height == 0) {
                SPDLOG_WARN("Session '{}': invalid DeviceInfo {}x{}",
                            i_->id, info.width, info.height);
                continue;
            }
            // Server renders at info.width/2; pixelsPerPt is in the
            // server's render coords, so device-pt → server-px is
            // pixelRatio / 2 (i.e. pixelRatio for full-res, halved
            // because we halve the surface).
            i_->pixelsPerPt = info.pixelRatio > 0
                ? float(info.pixelRatio) * 0.5f : 1.0f;
            switch (info.deviceClass) {
                case 1: i_->deviceClass = DeviceClass::Phone; break;
                case 2: i_->deviceClass = DeviceClass::Tablet; break;
                case 3: i_->deviceClass = DeviceClass::Desktop; break;
                default: i_->deviceClass = DeviceClass::Unknown; break;
            }
            i_->dimensionsKnown = true;
            SPDLOG_INFO("Session '{}': DeviceInfo {}x{} @{}x → render at {}x{}",
                        i_->id, info.width, info.height, info.pixelRatio,
                        i_->width, i_->height);
        } else if (magic == wire::kSdlEventMagic && i_->ready &&
                   data.size() >= sizeof(wire::MessageHeader) + sizeof(SDL_Event)) {
            SDL_Event ev;
            std::memcpy(&ev, data.data() + sizeof(wire::MessageHeader), sizeof(SDL_Event));
            if (i_->eventHandler) i_->eventHandler(ev);
        }
    }
    return i_->wire->isOpen();
}

void ServerWireBridge::pumpEvents() {
    pumpWire();
}

void ServerWireBridge::initialize() {
    if (!i_->dimensionsKnown) return;

    i_->renderTex = bgfx::createTexture2D(
        i_->width, i_->height, false, 1,
        bgfx::TextureFormat::BGRA8,
        BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
    bgfx::TextureHandle attachments[] = { i_->renderTex };
    i_->fb = bgfx::createFrameBuffer(1, attachments, false);

    size_t frameBytes = size_t(i_->width) * i_->height * 4;
    for (auto& slot : i_->readback) {
        slot.tex = bgfx::createTexture2D(
            i_->width, i_->height, false, 1,
            bgfx::TextureFormat::BGRA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        slot.buf.resize(frameBytes);
    }
    i_->pixelBuf.resize(frameBytes);

    auto wire = i_->wire;
    auto seq = i_->encodeSeq;
    i_->encoder = std::make_unique<VideoEncoder>(i_->width, i_->height, 60,
        [wire, seq](VideoEncoder::Frame frame) {
            if (!wire || !wire->isOpen()) return;
            uint32_t s = (*seq)++;
            uint8_t flags = frame.isKeyframe ? 1 : 0;
            uint32_t payloadSize = sizeof(flags) + sizeof(s) + frame.size;
            WireWriter w(sizeof(wire::MessageHeader) + payloadSize);
            w.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
            w.put(flags);
            w.put(s);
            w.append(frame.data, frame.size);
            wire->sendBinary(w.data(), w.size());
        });

    // Build the session Context now that dimensions are known.
    // Brokered mode uses :memory: — persistence belongs to the player
    // and is synced via sqlpipe.
    i_->ctx.emplace(i_->width, i_->height, i_->deviceClass,
                    ":memory:", i_->schemaDdl);
    i_->ctx->setPixelsPerPt(i_->pixelsPerPt);
    {
        // Same form-factor formula as DirectRenderHost: sqrt of the
        // device's short-side mm relative to the iPhone-PM reference.
        constexpr float kReferenceShortSideMm = 65.0f;
        constexpr float inchToMm = 25.4f;
        const float ptToMm = (i_->deviceClass == DeviceClass::Tablet)
            ? inchToMm / 132.0f : inchToMm / 163.0f;
        const float shortSidePt =
            float(std::min(i_->width, i_->height)) / i_->pixelsPerPt;
        const float scale = (i_->deviceClass == DeviceClass::Desktop)
            ? 1.0f
            : std::sqrt(shortSidePt * ptToMm / kReferenceShortSideMm);
        i_->ctx->setDeviceUiScale(scale);
    }

    i_->ready = true;
    SPDLOG_INFO("Session '{}': ready ({}x{})", i_->id, i_->width, i_->height);
}

void ServerWireBridge::beginFrame() {
    if (!i_->ready) return;
    // All bgfx views render into this session's framebuffer.
    for (bgfx::ViewId v = 0; v < 16; ++v) {
        bgfx::setViewFrameBuffer(v, i_->fb);
    }
    // Refresh per-frame Context state. Insets are zero in wire mode
    // until the player→server SafeAreaUpdate plumbing lands (🎯T37
    // follow-up); apps still consume the rect accessors identically
    // in both modalities.
    if (i_->ctx) {
        i_->ctx->setSurfaceDimensions(i_->width, i_->height);
        i_->ctx->setDrawSafeInsets(drawSafeInsetsInPts());
        i_->ctx->setUiSafeInsets(uiSafeInsetsInPts());
    }
}

void ServerWireBridge::endFrame(uint32_t bgfxFrameNumber) {
    if (!i_->ready) return;
    i_->submitCaptureBlit();
    if (i_->readCapturedFrame(bgfxFrameNumber)) {
        i_->encoder->encode(i_->pixelBuf.data(), i_->width * 4);
    }
}

void ServerWireBridge::shutdown() {
    if (i_->encoder) i_->encoder->flush();
    for (auto& slot : i_->readback) {
        if (bgfx::isValid(slot.tex)) {
            bgfx::destroy(slot.tex);
            slot.tex = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(i_->fb)) {
        bgfx::destroy(i_->fb);
        i_->fb = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(i_->renderTex)) {
        bgfx::destroy(i_->renderTex);
        i_->renderTex = BGFX_INVALID_HANDLE;
    }
    i_->ready = false;
}

} // namespace ge
