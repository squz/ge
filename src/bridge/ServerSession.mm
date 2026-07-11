// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerSession.h"

#include <ge/Protocol.h>
#include <ge/VideoEncoder.h>
#include <ge/WebSocketClient.h>
#include <ge/appchannel.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <unistd.h>  // getpid

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
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

    std::unique_ptr<VideoEncoder> encoder;           // legacy full-frame
    std::unique_ptr<TiledVideoEncoder> tiledEncoder; // 🎯T151 MTU tiles
    bool useTiles = true;
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

    // 🎯T149 encode-side stream telemetry (spyder session id for grepping).
    std::mutex sessionMu;
    std::string sessionId;
    std::mutex encodeStatsMu;
    struct EncodeWindow {
        uint64_t frames = 0;
        uint64_t keyframes = 0;
        uint64_t pframes = 0;
        uint64_t keyBytes = 0;
        uint64_t pBytes = 0;
        size_t maxKey = 0;
        size_t maxP = 0;
        std::chrono::steady_clock::time_point windowStart{};
    } encodeWin;
    nlohmann::json lastStreamStats = nlohmann::json::object();

    void sidebandLoop();
    void openWire(const std::string& sessionId);
    void closeWire();
    void inputLoop();
    void onEncoded(VideoEncoder::Frame f);  // VideoToolbox thread (legacy)
    void onTiled(TiledVideoEncoder::TileFrame t);
    void noteEncoded(size_t size, bool isKeyframe);
    void noteEncoded(const VideoEncoder::Frame& f) {
        noteEncoded(f.size, f.isKeyframe);
    }
    void flushEncodeStats(const char* reason);
    void publishEncodeStats(const EncodeWindow& snap, double windowSec,
                            const char* reason);

    void sendWire(uint32_t magic, const void* payload, uint32_t payloadLen) {
        wire::MessageHeader hdr{magic, payloadLen};
        std::vector<uint8_t> msg(sizeof(hdr) + payloadLen);
        std::memcpy(msg.data(), &hdr, sizeof(hdr));
        if (payloadLen) std::memcpy(msg.data() + sizeof(hdr), payload, payloadLen);
        std::lock_guard<std::mutex> lk(wireSendMu);
        if (wire && wire->isOpen()) wire->sendBinary(msg.data(), msg.size());
    }
};

// Publish one encode window via app-channel (MessagePack on the wire) + spdlog.
// 🎯T149 — identity is the app-channel session (spyder connection), not a field
// in the payload. Optional stream_session is only the relay pipe id (sN) for
// joining /stream/sessions when useful.
void ServerSession::Impl::publishEncodeStats(const EncodeWindow& snap, double windowSec,
                                             const char* reason) {
    std::string streamSession;
    {
        std::lock_guard sl(sessionMu);
        streamSession = sessionId;
    }
    const double sec = windowSec > 0 ? windowSec : 1.0;
    const double fps = snap.frames / sec;
    const double bps = double(snap.keyBytes + snap.pBytes) / sec;
    const size_t avgKey =
        snap.keyframes ? size_t(snap.keyBytes / snap.keyframes) : 0;
    const size_t avgP = snap.pframes ? size_t(snap.pBytes / snap.pframes) : 0;

    // Compact keys (msgpack): hot path is ~1 Hz, still keep names short.
    nlohmann::json j = {
        {"role", "server"},
        {"name", name},
        {"w", encW},
        {"h", encH},
        {"dt", sec},
        {"fps", fps},
        {"bps", bps},
        {"n", snap.frames},
        {"nk", snap.keyframes},
        {"np", snap.pframes},
        {"ak", avgKey},
        {"mk", snap.maxKey},
        {"ap", avgP},
        {"mp", snap.maxP},
    };
    if (!streamSession.empty()) j["sid"] = streamSession;  // relay pipe id, optional
    if (reason && *reason) j["why"] = reason;

    // Scalars for dashboard / app_perf_get (tightest recurring path).
    ge::appchannel::perfEmit("stream_fps", fps);
    ge::appchannel::perfEmit("stream_bps", bps);
    ge::appchannel::perfEmit("stream_mk", double(snap.maxKey));
    ge::appchannel::perfEmit("stream_w", double(encW));
    ge::appchannel::perfEmit("stream_h", double(encH));

    {
        std::lock_guard lock(encodeStatsMu);
        lastStreamStats = j;
    }
    ge::appchannel::push("stream_stats", j);

    SPDLOG_INFO(
        "StreamStats name={} sid={} encode={}x{} fps={:.1f} bps={:.0f} "
        "n={} nk={} np={} ak={} mk={} ap={} mp={}",
        name, streamSession.empty() ? "-" : streamSession, encW, encH, fps, bps,
        snap.frames, snap.keyframes, snap.pframes, avgKey, snap.maxKey, avgP,
        snap.maxP);
}

void ServerSession::Impl::noteEncoded(size_t size, bool isKeyframe) {
    const auto now = std::chrono::steady_clock::now();
    EncodeWindow snap{};
    double windowSec = 0;
    bool publish = false;
    {
        std::lock_guard lock(encodeStatsMu);
        if (encodeWin.windowStart.time_since_epoch().count() == 0)
            encodeWin.windowStart = now;
        encodeWin.frames++;
        if (isKeyframe) {
            encodeWin.keyframes++;
            encodeWin.keyBytes += size;
            if (size > encodeWin.maxKey) encodeWin.maxKey = size;
        } else {
            encodeWin.pframes++;
            encodeWin.pBytes += size;
            if (size > encodeWin.maxP) encodeWin.maxP = size;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - encodeWin.windowStart)
                            .count();
        if (ms >= 2000) {
            snap = encodeWin;
            windowSec = ms / 1000.0;
            encodeWin = {};
            encodeWin.windowStart = now;
            publish = true;
        }
    }
    if (publish) publishEncodeStats(snap, windowSec, nullptr);
}

void ServerSession::Impl::flushEncodeStats(const char* reason) {
    EncodeWindow snap{};
    double windowSec = 1.0;
    {
        std::lock_guard lock(encodeStatsMu);
        if (encodeWin.frames == 0) return;
        const auto now = std::chrono::steady_clock::now();
        const auto ms =
            encodeWin.windowStart.time_since_epoch().count() == 0
                ? 0
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - encodeWin.windowStart)
                      .count();
        snap = encodeWin;
        windowSec = ms > 0 ? ms / 1000.0 : 1.0;
        encodeWin = {};
    }
    publishEncodeStats(snap, windowSec, reason);
}

void ServerSession::Impl::onEncoded(VideoEncoder::Frame f) {
    if (!wire || !wire->isOpen() || !f.data || f.size == 0) return;
    noteEncoded(f);
    uint32_t s = seq.fetch_add(1);
    uint8_t flags = f.isKeyframe ? wire::kVideoFlagKeyframe : 0;
    uint32_t payloadSize = uint32_t(sizeof(flags) + sizeof(s) + f.size);
    WireWriter w(sizeof(wire::MessageHeader) + payloadSize);
    w.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
    w.put(flags);
    w.put(s);
    w.append(f.data, f.size);
    std::lock_guard<std::mutex> lk(wireSendMu);
    if (wire && wire->isOpen()) wire->sendBinary(w.data(), w.size());
}

void ServerSession::Impl::onTiled(TiledVideoEncoder::TileFrame t) {
    if (!wire || !wire->isOpen()) return;
    if (!t.blank) noteEncoded(t.size, t.isKeyframe);
    else noteEncoded(0, t.isKeyframe);

    uint8_t flags = wire::kVideoFlagTiled;
    if (t.isKeyframe) flags |= wire::kVideoFlagKeyframe;
    if (t.blank) flags |= wire::kVideoFlagBlank;

    // flags(1)+seq(4)+tile(2)+cols(1)+rows(1)+fw(2)+fh(2)+edge(2) = 15; + avcc
    const uint32_t header = 15;
    const uint32_t avcc = t.blank ? 0u : uint32_t(t.size);
    const uint32_t payloadSize = header + avcc;
    WireWriter w(sizeof(wire::MessageHeader) + payloadSize);
    w.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
    w.put(flags);
    w.put(t.frameSeq);
    w.put(t.tileId);
    w.put(t.cols);
    w.put(t.rows);
    w.put(t.frameW);
    w.put(t.frameH);
    w.put(t.tileEdge);
    if (!t.blank && t.data && t.size)
        w.append(t.data, t.size);
    std::lock_guard<std::mutex> lk(wireSendMu);
    if (wire && wire->isOpen()) wire->sendBinary(w.data(), w.size());
}

void ServerSession::Impl::openWire(const std::string& sessionId) {
    wire = connectWebSocket(host, port, "/ws/server/wire/" + sessionId, 2000);
    if (!wire || !wire->isOpen()) {
        SPDLOG_ERROR("ServerSession: wire open failed for session {}", sessionId);
        return;
    }
    {
        std::lock_guard lock(sessionMu);
        this->sessionId = sessionId;
    }
    {
        std::lock_guard lock(encodeStatsMu);
        encodeWin = {};
        encodeWin.windowStart = std::chrono::steady_clock::now();
    }
    hasPlayer.store(true);

    // Advertise session requirements (sensors, orientation) BEFORE the player
    // creates its window — the player blocks on this in PlayerWireBridge::connect.
    sendWire(wire::kSessionConfigMagic, &sessionConfig, sizeof(sessionConfig));
    // 🎯T149: same id spyder logs as session= — greppable across hops.
    sendWire(wire::kStreamSessionIdMagic, sessionId.data(),
             uint32_t(sessionId.size()));

    inputThread = std::thread([this] { inputLoop(); });
    SPDLOG_INFO("ServerSession: player attached (session {}), streaming", sessionId);
}

void ServerSession::Impl::closeWire() {
    if (!hasPlayer.exchange(false)) return;
    flushEncodeStats("player_detached");
    if (encoder) {
        encoder->flush();
        encoder.reset();
    }
    encW = encH = 0;
    if (wire) wire->close();
    if (inputThread.joinable()) inputThread.join();
    wire.reset();
    {
        std::lock_guard lock(sessionMu);
        sessionId.clear();
    }
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
    // 🎯T149: pull path for spyder (app_state slice=stream). Safe to register
    // after installFromEnv — state_query resolves by name on the live registry.
    ge::appchannel::registerStateSlice(
        "stream",
        [this]() -> nlohmann::json {
            std::lock_guard lock(i_->encodeStatsMu);
            if (i_->lastStreamStats.empty()) {
                std::string sid;
                {
                    std::lock_guard sl(i_->sessionMu);
                    sid = i_->sessionId;
                }
                nlohmann::json o{
                    {"role", "server"},
                    {"name", i_->name},
                    {"w", i_->encW},
                    {"h", i_->encH},
                    {"on", i_->hasPlayer.load()},
                };
                if (!sid.empty()) o["sid"] = sid;
                return o;
            }
            return i_->lastStreamStats;
        },
        nlohmann::json{
            {"role", "server"},
            {"name", "tiltbuggy"},
            {"w", 1024},
            {"h", 768},
            {"fps", 60.0},
            {"bps", 1000000.0},
            {"mk", 100000},
        });

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
    if (i_->encW != w || i_->encH != h) {
        i_->encW = w;
        i_->encH = h;
        i_->encoder.reset();
        i_->tiledEncoder.reset();
    }

    // 🎯T151: MTU-capped tiles by default. GE_STREAM_TILES=0 → legacy full-frame.
    static const bool tilesEnv = [] {
        const char* e = std::getenv("GE_STREAM_TILES");
        if (!e) return true;
        return std::strcmp(e, "0") != 0 && std::strcmp(e, "false") != 0;
    }();
    i_->useTiles = tilesEnv;

    if (i_->useTiles) {
        if (!i_->tiledEncoder) {
            TiledVideoEncoder::Config tc;
            tc.width = w;
            tc.height = h;
            tc.fps = 60;
            if (const char* e = std::getenv("GE_STREAM_MTU")) {
                int v = std::atoi(e);
                if (v > 0) tc.mtuBudget = v;
            }
            if (const char* e = std::getenv("GE_STREAM_TILE")) {
                int v = std::atoi(e);
                if (v >= 16) tc.preferredTileEdge = v;
            }
            if (const char* e = std::getenv("GE_STREAM_BPS")) {
                int v = std::atoi(e);
                if (v > 0) tc.totalAverageBitRate = v;
            }
            i_->tiledEncoder = std::make_unique<TiledVideoEncoder>(
                tc, [this](TiledVideoEncoder::TileFrame t) { i_->onTiled(t); });
        }
        i_->tiledEncoder->encode(px, static_cast<size_t>(w) * 4);
        return;
    }

    if (!i_->encoder) {
        i_->encoder = std::make_unique<VideoEncoder>(
            w, h, 60, [this](VideoEncoder::Frame f) { i_->onEncoded(f); });
    }
    i_->encoder->encode(px, static_cast<size_t>(w) * 4);
}

} // namespace ge
