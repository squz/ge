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
    cmdstream::Hash lastPresentHash{}; // skip wire when framebuffer unchanged
    bool haveLastPresentHash = false;
    // Present path is interim (full framebuffer, not draw-list). Cap rate and
    // max edge so Wi-Fi is not ~0.2 fps at 2048×1536 × ~11 MB/frame.
    static constexpr int kPresentMaxEdge = 960;       // long edge
    static constexpr double kPresentMinIntervalSec = 1.0 / 20.0; // 20 Hz cap
    uint64_t lastPresentTicks = 0;

    void sidebandLoop();
    void openWire(const std::string& sessionId);
    void closeWire();
    void inputLoop();
    void onEncoded(VideoEncoder::Frame f);  // VideoToolbox thread
    void negotiateTransport(uint8_t playerCaps);
    void sendCmdStreamSpikeFrame(uint32_t seq, bool first);

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
    haveLastPresentHash = false;
    lastPresentHash = {};
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

    // Spike: when cmdstream is selected, emit one synthetic GE2S frame so the
    // player/relay path is exercised without a full sokol capture backend yet.
    if (next == wire::kTransportCommandStream) {
        sendCmdStreamSpikeFrame(/*seq*/ 0, /*first*/ true);
    }
}

void ServerSession::Impl::sendCmdStreamSpikeFrame(uint32_t seq, bool first) {
    cmdstream::Writer w(&cmdCache);
    auto scene = cmdstream::SyntheticScene::tiltbuggyLike();
    scene.writeFrame(w, seq, first);
    auto payload = w.take();
    sendWire(wire::kCommandStreamMagic, payload.data(),
             static_cast<uint32_t>(payload.size()));
    SPDLOG_INFO("ServerSession: GE2S spike frame seq={} bytes={} full_blobs={} refs={}",
                seq, payload.size(), w.stats().fullBlobCount, w.stats().refBlobCount);
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

    // 🎯T128 runnable path: GE2S Present of the captured framebuffer with
    // LZ4 + content-addressed cache. Works with the existing SDL player
    // (no sokol on the player). Full draw-list remoting remains T128.2.1/2.2.
    if (i_->transport.load() == wire::kTransportCommandStream) {
        // Rate limit: capture runs at display rate; Present is multi-MB.
        const uint64_t now = SDL_GetPerformanceCounter();
        const uint64_t freq = SDL_GetPerformanceFrequency();
        if (i_->lastPresentTicks != 0) {
            const double dt = double(now - i_->lastPresentTicks) / double(freq);
            if (dt < Impl::kPresentMinIntervalSec) return;
        }

        // Downscale so long edge ≤ kPresentMaxEdge (box filter). 2048×1536 →
        // 960×720 ≈ 2.6 MB raw / often <1 MB LZ4 vs ~11 MB full-res.
        int dw = w, dh = h;
        std::vector<uint8_t> scaled;
        const uint8_t* sendPx = px;
        if (w > Impl::kPresentMaxEdge || h > Impl::kPresentMaxEdge) {
            const float scale = float(Impl::kPresentMaxEdge) /
                                float(std::max(w, h));
            dw = std::max(1, int(w * scale));
            dh = std::max(1, int(h * scale));
            scaled.resize(static_cast<size_t>(dw) * dh * 4);
            for (int y = 0; y < dh; ++y) {
                const int sy = y * h / dh;
                for (int x = 0; x < dw; ++x) {
                    const int sx = x * w / dw;
                    const size_t di = (static_cast<size_t>(y) * dw + x) * 4;
                    const size_t si = (static_cast<size_t>(sy) * w + sx) * 4;
                    scaled[di + 0] = px[si + 0];
                    scaled[di + 1] = px[si + 1];
                    scaled[di + 2] = px[si + 2];
                    scaled[di + 3] = px[si + 3];
                }
            }
            sendPx = scaled.data();
        }
        const size_t raw = static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4;

        // Skip identical frames entirely (player keeps last texture).
        const cmdstream::Hash frameHash = cmdstream::hashBytes(sendPx, raw);
        if (i_->haveLastPresentHash && frameHash == i_->lastPresentHash)
            return;
        i_->lastPresentHash = frameHash;
        i_->haveLastPresentHash = true;
        i_->lastPresentTicks = now;

        cmdstream::Writer wr(&i_->cmdCache);
        const uint32_t s = i_->seq.fetch_add(1);
        wr.frameBegin(s, /*fullState*/ false);
        // Capture sink contract is RGBA8 (SokolContext swizzles Metal BGRA→RGBA).
        wr.present(static_cast<uint16_t>(dw), static_cast<uint16_t>(dh),
                   cmdstream::kPresentRGBA8, sendPx, raw);
        wr.frameEnd();
        auto payload = wr.take();
        i_->sendWire(wire::kCommandStreamMagic, payload.data(),
                     static_cast<uint32_t>(payload.size()));
        if (s < 3 || (s % 30) == 0) {
            SPDLOG_INFO("ServerSession: GE2S Present seq={} {}x{} (src {}x{}) "
                        "wire={}B full_blobs={} refs={} full_blob_bytes={}",
                        s, dw, dh, w, h, payload.size(),
                        wr.stats().fullBlobCount, wr.stats().refBlobCount,
                        wr.stats().fullBlobBytes);
        }
        return;
    }

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
