// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerSession.h"

#include <ge/CmdStream.h>
#include <ge/Protocol.h>
#include <ge/VideoEncoder.h>
#include <ge/WebSocketClient.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <unistd.h>  // getpid

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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

    // 🎯T128.9 — negotiated transport. Defaults H.264; upgraded when the
    // player advertises kCapCommandStream and the server opts in
    // (GE_TRANSPORT=cmdstream). SessionConfig is re-sent after negotiation.
    std::atomic<uint8_t> transport{wire::kTransportH264};
    cmdstream::Cache cmdCache; // server-side "already sent" memo for GE2S
    cmdstream::LiveCapture live; // sprite-run capture under cmdstream
    // false when cmdstream is selected — skip multi-MB GPU readback.
    std::atomic<bool> capturePixels{true};
    // Legacy Present path (not used for 60 fps goal).

    void sidebandLoop();
    void openWire(const std::string& sessionId);
    void closeWire();
    void inputLoop();
    void onEncoded(VideoEncoder::Frame f);  // VideoToolbox thread
    void negotiateTransport(uint8_t playerCaps);

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
    transport.store(wire::kTransportH264);
    capturePixels.store(true);
    cmdstream::setLiveCapture(nullptr);
    live.resetSession();
    cmdCache.clear();
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
                   data.size() >= sizeof(wire::MessageHeader) + 14) {
            // DeviceInfo: at least pre-v7 fields (through orientation). v7 adds
            // capabilities; short payloads default capabilities=0.
            wire::DeviceInfo info{};
            const size_t avail = data.size() - sizeof(wire::MessageHeader);
            std::memcpy(&info, data.data() + sizeof(wire::MessageHeader),
                        std::min(avail, sizeof(info)));
            deviceClass.store(info.deviceClass);
            pixelRatio.store(info.pixelRatio);
            SPDLOG_INFO("ServerSession: player DeviceInfo {}x{} @{}x class={} caps={:#x}",
                        info.width, info.height, info.pixelRatio, info.deviceClass,
                        info.capabilities);
            negotiateTransport(info.capabilities);
        }
    }
}

void ServerSession::Impl::negotiateTransport(uint8_t playerCaps) {
    const bool playerCmd = (playerCaps & wire::kCapCommandStream) != 0;
    const char* pref = std::getenv("GE_TRANSPORT");
    // Default remains H.264. Opt into command-stream only when the player
    // can replay and the server was launched with GE_TRANSPORT=cmdstream.
    const bool wantCmd = pref && std::strcmp(pref, "cmdstream") == 0;
    uint8_t next = wire::kTransportH264;
    if (playerCmd && wantCmd) next = wire::kTransportCommandStream;
    transport.store(next);
    sessionConfig.transport = next;
    sendWire(wire::kSessionConfigMagic, &sessionConfig, sizeof(sessionConfig));
    SPDLOG_INFO("ServerSession: transport={} (player_cmdstream={} want={})",
                next == wire::kTransportCommandStream ? "cmdstream" : "h264",
                playerCmd, wantCmd);

    // Pixel GPU readback only needed for H.264 / legacy Present.
    capturePixels.store(next != wire::kTransportCommandStream);
    if (next == wire::kTransportCommandStream) {
        SPDLOG_INFO("ServerSession: cmdstream LiveCapture armed (sprite runs)");
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

    // Command-stream uses LiveCapture (onFrameBegin/End), not pixel Present.
    if (i_->transport.load() == wire::kTransportCommandStream) return;

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

std::atomic<bool>* ServerSession::capturePixelsFlag() { return &i_->capturePixels; }

void ServerSession::onFrameBegin(int contentW, int contentH) {
    if (!i_->hasPlayer.load()) return;
    if (i_->transport.load() != wire::kTransportCommandStream) return;
    const uint32_t s = i_->seq.fetch_add(1);
    const uint16_t cw = contentW > 0 ? static_cast<uint16_t>(
        std::min(contentW, 65535)) : 0;
    const uint16_t ch = contentH > 0 ? static_cast<uint16_t>(
        std::min(contentH, 65535)) : 0;
    i_->live.begin(s, &i_->cmdCache, cw, ch);
    cmdstream::setLiveCapture(&i_->live);
}

void ServerSession::onFrameEnd() {
    if (cmdstream::liveCapture() != &i_->live) return;
    cmdstream::setLiveCapture(nullptr);
    auto payload = i_->live.end();
    if (payload.empty()) return;
    const auto st = i_->live.stats();
    i_->sendWire(wire::kCommandStreamMagic, payload.data(),
                 static_cast<uint32_t>(payload.size()));
    // Bandwidth telemetry: every frame when GE_CMDSTREAM_BW_LOG=1, else
    // first 5 frames + every 60th (steady-state sample).
    static uint32_t logN = 0;
    const uint32_t n = ++logN;
    static const bool every =
        [] {
            const char* e = std::getenv("GE_CMDSTREAM_BW_LOG");
            return e && e[0] == '1';
        }();
    if (every || n <= 5 || (n % 60) == 0) {
        SPDLOG_INFO("ServerSession: GE2S frame#{} wire={}B runs={} "
                    "full_blobs={} refs={} full_blob_bytes={}",
                    n, payload.size(), i_->live.runCount(),
                    st.fullBlobCount, st.refBlobCount, st.fullBlobBytes);
    }
}

} // namespace ge
