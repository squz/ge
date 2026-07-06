// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerSession.h"

#include <ge/Protocol.h>
#include <ge/VideoEncoder.h>
#include <ge/WebSocketClient.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <unistd.h>  // getpid

#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ge {

namespace {

// Minimal JSON string-field extractor (moved from StreamClient).
std::string jsonStr(const std::string& json, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    auto p = json.find(k);
    if (p == std::string::npos) return "";
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";
    auto e = json.find('"', p + 1);
    if (e == std::string::npos) return "";
    return json.substr(p + 1, e - (p + 1));
}

// Lightweight write buffer for building wire messages (from ServerWireBridge).
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

struct ServerSession::Impl {
    std::string host;
    int port = 0;
    std::string name;
    wire::SessionConfig sessionConfig{};

    std::shared_ptr<WsConnection> sideband;
    std::shared_ptr<WsConnection> wire;
    std::mutex wireSendMu;

    std::unique_ptr<VideoEncoder> encoder;
    int encW = 0, encH = 0;
    std::atomic<uint32_t> seq{0};

    std::atomic<bool> running{false};
    std::atomic<bool> hasPlayer{false};
    std::thread sidebandThread;
    std::thread inputThread;

    // Player-advertised device hints, parsed from DeviceInfo on the input
    // thread. Used only for deviceClass / pixelRatio context; NEVER for encode
    // dims (those are the captured frame's actual w×h — see onCapturedFrame).
    std::atomic<int> deviceClass{0};
    std::atomic<int> pixelRatio{0};

    void sidebandLoop();
    void openWire(const std::string& sessionId);
    void closeWire();
    void inputLoop();
    void onEncoded(VideoEncoder::Frame f);  // VideoToolbox thread

    void sendWire(uint32_t magic, const void* payload, uint32_t payloadLen) {
        wire::MessageHeader hdr{magic, payloadLen};
        std::vector<uint8_t> msg(sizeof(hdr) + payloadLen);
        std::memcpy(msg.data(), &hdr, sizeof(hdr));
        if (payloadLen) std::memcpy(msg.data() + sizeof(hdr), payload, payloadLen);
        std::lock_guard<std::mutex> lk(wireSendMu);
        if (wire && wire->isOpen()) wire->sendBinary(msg.data(), msg.size());
    }
};

void ServerSession::Impl::onEncoded(VideoEncoder::Frame f) {
    if (!wire || !wire->isOpen() || !f.data || f.size == 0) return;
    uint32_t s = seq.fetch_add(1);
    uint8_t flags = f.isKeyframe ? 1 : 0;
    uint32_t payloadSize = uint32_t(sizeof(flags) + sizeof(s) + f.size);
    WireWriter w(sizeof(wire::MessageHeader) + payloadSize);
    w.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
    w.put(flags);
    w.put(s);
    w.append(f.data, f.size);
    std::lock_guard<std::mutex> lk(wireSendMu);
    if (wire && wire->isOpen()) wire->sendBinary(w.data(), w.size());
}

void ServerSession::Impl::openWire(const std::string& sessionId) {
    wire = connectWebSocket(host, port, "/ws/server/wire/" + sessionId, 2000);
    if (!wire || !wire->isOpen()) {
        SPDLOG_ERROR("ServerSession: wire open failed for session {}", sessionId);
        return;
    }
    hasPlayer.store(true);

    // Advertise session requirements (sensors, orientation) BEFORE the player
    // creates its window — the player blocks on this in PlayerWireBridge::connect.
    sendWire(wire::kSessionConfigMagic, &sessionConfig, sizeof(sessionConfig));

    inputThread = std::thread([this] { inputLoop(); });
    SPDLOG_INFO("ServerSession: player attached (session {}), streaming", sessionId);
}

void ServerSession::Impl::closeWire() {
    if (!hasPlayer.exchange(false)) return;
    if (encoder) {
        encoder->flush();
        encoder.reset();
    }
    encW = encH = 0;
    if (wire) wire->close();
    if (inputThread.joinable()) inputThread.join();
    wire.reset();
    SPDLOG_INFO("ServerSession: player detached");
}

void ServerSession::Impl::inputLoop() {
    while (running.load() && wire && wire->isOpen()) {
        std::vector<char> data;
        if (!wire->recvBinary(data)) break;
        if (data.size() < sizeof(wire::MessageHeader)) continue;
        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), 4);
        if (magic == wire::kSdlEventMagic &&
            data.size() >= sizeof(wire::MessageHeader) + sizeof(SDL_Event)) {
            SDL_Event ev;
            std::memcpy(&ev, data.data() + sizeof(wire::MessageHeader), sizeof(SDL_Event));
            SDL_PushEvent(&ev);
        } else if (magic == wire::kDeviceInfoMagic &&
                   data.size() >= sizeof(wire::MessageHeader) + sizeof(wire::DeviceInfo)) {
            // DeviceInfo gives the player's deviceClass / pixelRatio only — a
            // hint for downstream context. It does NOT drive encode dims: the
            // server renders at SessionHostConfig.width/height and streams at
            // the captured frame's actual w×h (see onCapturedFrame). We do not
            // resize the encoder or the render surface from it.
            wire::DeviceInfo info;
            std::memcpy(&info, data.data() + sizeof(wire::MessageHeader), sizeof(info));
            deviceClass.store(info.deviceClass);
            pixelRatio.store(info.pixelRatio);
            SPDLOG_INFO("ServerSession: player DeviceInfo {}x{} @{}x class={} (hints only)",
                        info.width, info.height, info.pixelRatio, info.deviceClass);
        }
    }
}

void ServerSession::Impl::sidebandLoop() {
    sideband = connectWebSocket(host, port, "/ws/server?name=" + name, 2000);
    if (!sideband || !sideband->isOpen()) {
        SPDLOG_WARN("ServerSession: relay connect {}:{} failed — not streaming", host, port);
        return;
    }
    const std::string hello =
        "{\"type\":\"hello\",\"name\":\"" + name + "\",\"pid\":" +
        std::to_string(getpid()) + ",\"version\":" +
        std::to_string(wire::kProtocolVersion) + "}";
    sideband->sendText(hello);
    SPDLOG_INFO("ServerSession: connected to relay {}:{} as '{}'", host, port, name);

    while (running.load() && sideband->isOpen()) {
        std::vector<char> data;
        if (!sideband->recvBinary(data) || data.empty()) break;
        if (data[0] != '{') continue;
        const std::string msg(data.begin(), data.end());
        const std::string type = jsonStr(msg, "type");
        const std::string sessionId = jsonStr(msg, "session_id");
        if (type == "player_attached" && !sessionId.empty() && !hasPlayer.load()) {
            openWire(sessionId);
        } else if (type == "player_detached") {
            closeWire();
        }
    }
    closeWire();
}

ServerSession::ServerSession(std::string host, int port, std::string name,
                             const wire::SessionConfig& cfg)
    : i_(std::make_unique<Impl>()) {
    i_->host = std::move(host);
    i_->port = port;
    i_->name = std::move(name);
    i_->sessionConfig = cfg;
    i_->sessionConfig.magic = wire::kSessionConfigMagic;
}

ServerSession::~ServerSession() { stop(); }

void ServerSession::start() {
    i_->running.store(true);
    i_->sidebandThread = std::thread([this] { i_->sidebandLoop(); });
}

void ServerSession::stop() {
    if (!i_->running.exchange(false)) return;
    if (i_->sideband) i_->sideband->close();
    if (i_->wire) i_->wire->close();
    if (i_->sidebandThread.joinable()) i_->sidebandThread.join();
    if (i_->inputThread.joinable()) i_->inputThread.join();
}

bool ServerSession::active() const { return i_->hasPlayer.load(); }

std::atomic<bool>* ServerSession::activeFlag() { return &i_->hasPlayer; }

void ServerSession::onCapturedFrame(const std::uint8_t* px, int w, int h) {
    if (!i_->hasPlayer.load() || !px || w <= 0 || h <= 0) return;
    // Encode at the CAPTURED frame's actual dimensions (StreamClient's original
    // behaviour): the frame we hand VideoToolbox is exactly what the swapchain
    // presented. DeviceInfo is a hint, not a resize trigger.
    if (!i_->encoder || i_->encW != w || i_->encH != h) {
        i_->encW = w;
        i_->encH = h;
        i_->encoder = std::make_unique<VideoEncoder>(
            w, h, 60, [this](VideoEncoder::Frame f) { i_->onEncoded(f); });
    }
    i_->encoder->encode(px, static_cast<size_t>(w) * 4);
}

} // namespace ge
