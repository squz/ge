// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerSession.h"

#include "../render/LifecycleInject.h"

#include <ge/CmdStream.h>
#include <ge/Protocol.h>
#include <ge/StreamHostPolicy.h>
#include <ge/VideoEncoder.h>
#include <ge/ViewerMetrics.h>
#include <ge/WebSocketClient.h>

#include <ge/audio.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <unistd.h>  // getpid

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
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

    // Player surface discovery (DeviceInfo / SafeAreaUpdate). Applied to game
    // Context by DirectRenderHost when streaming. Encode/cmdstream size remains
    // the server surface — never resized from DeviceInfo.
    ViewerMetricsStore viewer{};

    // 🎯T154 GE2T: host Context::db() working set (:memory: under stream).
    // Player dumps seed on attach; we push back on detach for durable store.
    std::shared_ptr<sqlpipe::Database> workingDb;
    std::mutex workingDbMu;

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

    // Advertise session requirements BEFORE the player creates its window —
    // the player blocks on this in PlayerWireBridge::connect (handshake step 1).
    sendWire(wire::kSessionConfigMagic, &sessionConfig, sizeof(sessionConfig));
    SPDLOG_INFO("ServerSession: SessionConfig sensors={:#x} orientation={} "
                "transport={} flags={:#x} (immersive={} noSaver={})",
                sessionConfig.sensors, sessionConfig.orientation,
                sessionConfig.transport, sessionConfig.flags,
                (sessionConfig.flags & wire::kSessionFlagImmersive) ? 1 : 0,
                (sessionConfig.flags & wire::kSessionFlagNoScreenSaver) ? 1 : 0);

    inputThread = std::thread([this] { inputLoop(); });
    SPDLOG_INFO("ServerSession: player attached (session {}), streaming", sessionId);
}

void ServerSession::Impl::closeWire() {
    if (!hasPlayer.exchange(false)) return;
    // 🎯T154 GE2T: push working set to player durable store before teardown.
    {
        std::lock_guard<std::mutex> lk(workingDbMu);
        if (workingDb && wire && wire->isOpen()) {
            std::vector<uint8_t> dump;
            if (dumpSqliteMain(workingDb->handle(), dump) && !dump.empty()) {
                sendWire(wire::kSqlpipeMsgMagic, dump.data(),
                         static_cast<uint32_t>(dump.size()));
                SPDLOG_INFO("ServerSession: GE2T snapshot pushed ({} bytes)",
                            dump.size());
            }
        }
    }
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
            // v7 payload ends before drawSafe*; short copies leave drawW=0.
            viewer.applyDeviceInfo(info.width, info.height, info.pixelRatio,
                                   info.deviceClass, info.safeX, info.safeY,
                                   info.safeW, info.safeH,
                                   info.drawSafeX, info.drawSafeY,
                                   info.drawSafeW, info.drawSafeH);
            SPDLOG_INFO("ServerSession: player DeviceInfo {}x{} @{}x class={} "
                        "ui=({},{} {}x{}) draw=({},{} {}x{}) caps={:#x}",
                        info.width, info.height, info.pixelRatio, info.deviceClass,
                        info.safeX, info.safeY, info.safeW, info.safeH,
                        info.drawSafeX, info.drawSafeY, info.drawSafeW, info.drawSafeH,
                        info.capabilities);
            negotiateTransport(info.capabilities);
        } else if (magic == wire::kSafeAreaMagic &&
                   data.size() >= sizeof(wire::MessageHeader) + 12) {
            // Min: magic+ui rect (4+8). v8 adds drawSafe*.
            wire::SafeAreaUpdate sa{};
            const size_t avail = data.size() - sizeof(wire::MessageHeader);
            std::memcpy(&sa, data.data() + sizeof(wire::MessageHeader),
                        std::min(avail, sizeof(sa)));
            viewer.applySafeArea(sa.safeX, sa.safeY, sa.safeW, sa.safeH,
                                 sa.drawSafeX, sa.drawSafeY, sa.drawSafeW, sa.drawSafeH);
            SPDLOG_INFO("ServerSession: player SafeAreaUpdate ui=({},{} {}x{}) "
                        "draw=({},{} {}x{})",
                        sa.safeX, sa.safeY, sa.safeW, sa.safeH,
                        sa.drawSafeX, sa.drawSafeY, sa.drawSafeW, sa.drawSafeH);
        } else if (magic == wire::kLifecycleMagic &&
                   data.size() >= sizeof(wire::MessageHeader) + 2) {
            wire::ViewerLifecycle life{};
            const size_t avail = data.size() - sizeof(wire::MessageHeader);
            std::memcpy(&life, data.data() + sizeof(wire::MessageHeader),
                        std::min(avail, sizeof(life)));
            switch (life.kind) {
            case wire::kLifeForeground:
                detail::injectViewerBackgrounded(false);
                ge::audio::onForeground();
                break;
            case wire::kLifeBackground:
                detail::injectViewerBackgrounded(true);
                ge::audio::onBackground();
                break;
            case wire::kLifeBackPressed:
                detail::injectBackPressed();
                break;
            case wire::kLifeMemory:
                detail::injectMemoryWarning(
                    static_cast<MemoryPressureLevel>(life.memoryLevel));
                break;
            case wire::kLifeAudioLost:
                detail::injectAudioFocus(false);
                ge::audio::onAudioFocusLost();
                break;
            case wire::kLifeAudioGained:
                detail::injectAudioFocus(true);
                ge::audio::onAudioFocusGained();
                break;
            default:
                break;
            }
            SPDLOG_INFO("ServerSession: viewer lifecycle kind={}", life.kind);
        } else if (magic == wire::kSqlpipeMsgMagic &&
                   data.size() > sizeof(wire::MessageHeader)) {
            // 🎯T154 GE2T: player → server durable seed into :memory: working set.
            const auto* payload = reinterpret_cast<const uint8_t*>(
                data.data() + sizeof(wire::MessageHeader));
            const size_t n = data.size() - sizeof(wire::MessageHeader);
            std::lock_guard<std::mutex> lk(workingDbMu);
            if (workingDb && n > 0 &&
                loadSqliteMain(workingDb->handle(),
                               std::span<const uint8_t>(payload, n))) {
                workingDb->notify();
                SPDLOG_INFO("ServerSession: GE2T snapshot applied ({} bytes)", n);
            } else {
                SPDLOG_WARN("ServerSession: GE2T snapshot apply failed "
                            "(db={} n={})", workingDb ? 1 : 0, n);
            }
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

ViewerMetricsStore* ServerSession::viewerMetrics() { return &i_->viewer; }

void ServerSession::setWorkingDb(std::shared_ptr<sqlpipe::Database> db) {
    std::lock_guard<std::mutex> lk(i_->workingDbMu);
    i_->workingDb = std::move(db);
    SPDLOG_INFO("ServerSession: GE2T working db bound ({})",
                i_->workingDb ? "ok" : "null");
}

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
