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

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ge {

namespace {

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

struct WireWriter {
    std::vector<uint8_t> buf;
    explicit WireWriter(size_t cap = 0) { buf.reserve(cap); }
    template <typename T>
    void put(const T& v) {
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

struct EncodeWindow {
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    uint64_t pframes = 0;
    uint64_t keyBytes = 0;
    uint64_t pBytes = 0;
    size_t maxKey = 0;
    size_t maxP = 0;
    std::chrono::steady_clock::time_point windowStart{};
};

// One independent player: wire + encoder + input queue.
struct PlayerSlot {
    std::string id;
    std::shared_ptr<WsConnection> wire;
    std::mutex sendMu;

    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<TiledVideoEncoder> tiledEncoder;
    bool useTiles = true;
    int encW = 0, encH = 0;
    int tileCount = 1;  // grid size for stats (1 = legacy full-frame)
    std::atomic<uint32_t> seq{0};

    std::thread inputThread;
    std::atomic<bool> inputRunning{true};
    std::mutex inputMu;
    std::vector<SDL_Event> inputQ;

    std::mutex statsMu;
    EncodeWindow encodeWin;
    nlohmann::json lastStreamStats = nlohmann::json::object();
};

} // namespace

struct ServerSession::Impl {
    std::string host;
    int port = 0;
    std::string name;
    wire::SessionConfig sessionConfig{};

    std::shared_ptr<WsConnection> sideband;
    std::atomic<bool> running{false};
    std::atomic<bool> anyPlayer{false};
    std::thread sidebandThread;

    mutable std::mutex slotsMu;
    // shared_ptr so encode can run without holding slotsMu across VT work.
    std::unordered_map<std::string, std::shared_ptr<PlayerSlot>> slots;

    std::mutex lifeMu;
    std::vector<std::string> pendingAttach;
    std::vector<std::string> pendingDetach;

    std::mutex targetMu;
    std::string captureTarget;

    void sidebandLoop();
    void openSlot(const std::string& sessionId);
    void closeSlot(const std::string& sessionId);
    void inputLoop(PlayerSlot* slot);
    void sendWire(PlayerSlot& slot, uint32_t magic, const void* payload,
                  uint32_t payloadLen);
    void noteEncoded(PlayerSlot& slot, size_t size, bool isKeyframe);
    void publishEncodeStats(PlayerSlot& slot, const EncodeWindow& snap,
                            double windowSec, const char* reason);
    void onEncoded(PlayerSlot& slot, VideoEncoder::Frame f);
    void onTiled(PlayerSlot& slot, TiledVideoEncoder::TileFrame t);
    void encodeFrame(PlayerSlot& slot, const uint8_t* px, int w, int h);
    void refreshAnyPlayer();
};

void ServerSession::Impl::refreshAnyPlayer() {
    std::lock_guard lock(slotsMu);
    anyPlayer.store(!slots.empty());
}

void ServerSession::Impl::sendWire(PlayerSlot& slot, uint32_t magic,
                                   const void* payload, uint32_t payloadLen) {
    wire::MessageHeader hdr{magic, payloadLen};
    std::vector<uint8_t> msg(sizeof(hdr) + payloadLen);
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    if (payloadLen) std::memcpy(msg.data() + sizeof(hdr), payload, payloadLen);
    std::lock_guard<std::mutex> lk(slot.sendMu);
    if (slot.wire && slot.wire->isOpen())
        slot.wire->sendBinary(msg.data(), msg.size());
}

void ServerSession::Impl::noteEncoded(PlayerSlot& slot, size_t size,
                                      bool isKeyframe) {
    const auto now = std::chrono::steady_clock::now();
    EncodeWindow snap{};
    double windowSec = 0;
    bool publish = false;
    {
        std::lock_guard lock(slot.statsMu);
        if (slot.encodeWin.windowStart.time_since_epoch().count() == 0)
            slot.encodeWin.windowStart = now;
        slot.encodeWin.frames++;
        if (isKeyframe) {
            slot.encodeWin.keyframes++;
            slot.encodeWin.keyBytes += size;
            if (size > slot.encodeWin.maxKey) slot.encodeWin.maxKey = size;
        } else {
            slot.encodeWin.pframes++;
            slot.encodeWin.pBytes += size;
            if (size > slot.encodeWin.maxP) slot.encodeWin.maxP = size;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - slot.encodeWin.windowStart)
                            .count();
        if (ms >= 2000) {
            snap = slot.encodeWin;
            windowSec = ms / 1000.0;
            slot.encodeWin = {};
            slot.encodeWin.windowStart = now;
            publish = true;
        }
    }
    if (publish) publishEncodeStats(slot, snap, windowSec, nullptr);
}

void ServerSession::Impl::publishEncodeStats(PlayerSlot& slot,
                                             const EncodeWindow& snap,
                                             double windowSec,
                                             const char* reason) {
    if (snap.frames == 0 || windowSec <= 0) return;
    // `fps` here is *tile/AU* rate (or full frames if untiled). Full-frame
    // rate for compression math is tile-rate / tileCount.
    const double auFps = snap.frames / windowSec;
    const int tiles = std::max(1, slot.tileCount);
    const double fullFps = auFps / double(tiles);
    const double bps =
        double(snap.keyBytes + snap.pBytes) * 8.0 / windowSec;
    // Uncompressed RGBA bit-rate at the full-frame cadence.
    const double rawBps =
        double(slot.encW) * double(slot.encH) * 32.0 * fullFps;
    const double comp =
        (bps > 1.0 && rawBps > 0.0) ? (rawBps / bps) : 0.0;
    const double avgKey =
        snap.keyframes ? double(snap.keyBytes) / snap.keyframes : 0;
    const double avgP =
        snap.pframes ? double(snap.pBytes) / snap.pframes : 0;
    nlohmann::json j{
        {"role", "server"},
        {"name", name},
        {"sid", slot.id},
        {"w", slot.encW},
        {"h", slot.encH},
        {"fps", fullFps},
        {"au_fps", auFps},
        {"tiles", tiles},
        {"bps", bps},
        {"raw_bps", rawBps},
        {"comp", comp},
        {"n", snap.frames},
        {"nk", snap.keyframes},
        {"np", snap.pframes},
        {"ak", avgKey},
        {"mk", snap.maxKey},
        {"ap", avgP},
        {"mp", snap.maxP},
        {"on", true},
    };
    if (reason) j["reason"] = reason;
    {
        std::lock_guard lock(slot.statsMu);
        slot.lastStreamStats = j;
    }
    ge::appchannel::perfEmit("stream_fps", fullFps);
    ge::appchannel::perfEmit("stream_bps", bps);
    ge::appchannel::perfEmit("stream_comp", comp);
    ge::appchannel::push("stream_stats", j);
    SPDLOG_INFO(
        "StreamStats name={} sid={} encode={}x{} full_fps={:.1f} tiles={} "
        "bps={:.0f} raw_bps={:.0f} comp={:.0f}x "
        "n={} nk={} np={} ak={:.0f} mk={} ap={:.0f} mp={}",
        name, slot.id, slot.encW, slot.encH, fullFps, tiles, bps, rawBps,
        comp, snap.frames, snap.keyframes, snap.pframes, avgKey, snap.maxKey,
        avgP, snap.maxP);
}

void ServerSession::Impl::onEncoded(PlayerSlot& slot, VideoEncoder::Frame f) {
    if (!slot.wire || !slot.wire->isOpen() || !f.data || f.size == 0) return;
    noteEncoded(slot, f.size, f.isKeyframe);
    uint32_t s = slot.seq.fetch_add(1);
    uint8_t flags = f.isKeyframe ? wire::kVideoFlagKeyframe : 0;
    uint32_t payloadSize = uint32_t(sizeof(flags) + sizeof(s) + f.size);
    WireWriter w(sizeof(wire::MessageHeader) + payloadSize);
    w.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
    w.put(flags);
    w.put(s);
    w.append(f.data, f.size);
    std::lock_guard<std::mutex> lk(slot.sendMu);
    if (slot.wire && slot.wire->isOpen())
        slot.wire->sendBinary(w.data(), w.size());
}

void ServerSession::Impl::onTiled(PlayerSlot& slot,
                                  TiledVideoEncoder::TileFrame t) {
    if (!slot.wire || !slot.wire->isOpen()) return;
    if (!t.blank) noteEncoded(slot, t.size, t.isKeyframe);
    else noteEncoded(slot, 0, t.isKeyframe);

    uint8_t flags = wire::kVideoFlagTiled;
    if (t.isKeyframe) flags |= wire::kVideoFlagKeyframe;
    if (t.blank) flags |= wire::kVideoFlagBlank;

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
    if (!t.blank && t.data && t.size) w.append(t.data, t.size);
    std::lock_guard<std::mutex> lk(slot.sendMu);
    if (slot.wire && slot.wire->isOpen())
        slot.wire->sendBinary(w.data(), w.size());
}

void ServerSession::Impl::encodeFrame(PlayerSlot& slot, const uint8_t* px,
                                      int w, int h) {
    if (!px || w <= 0 || h <= 0) return;
    if (slot.encW != w || slot.encH != h) {
        slot.encW = w;
        slot.encH = h;
        slot.encoder.reset();
        slot.tiledEncoder.reset();
    }

    static const bool tilesEnv = [] {
        const char* e = std::getenv("GE_STREAM_TILES");
        if (!e) return true;
        return std::strcmp(e, "0") != 0 && std::strcmp(e, "false") != 0;
    }();
    slot.useTiles = tilesEnv;

    if (slot.useTiles) {
        if (!slot.tiledEncoder) {
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
            if (const char* e = std::getenv("GE_STREAM_MAX_TILES")) {
                int v = std::atoi(e);
                if (v >= 1) tc.maxTiles = v;
            }
            if (const char* e = std::getenv("GE_STREAM_BPS")) {
                int v = std::atoi(e);
                if (v > 0) tc.totalAverageBitRate = v;
            }
            PlayerSlot* self = &slot;
            slot.tiledEncoder = std::make_unique<TiledVideoEncoder>(
                tc, [this, self](TiledVideoEncoder::TileFrame t) {
                    onTiled(*self, t);
                });
            slot.tileCount = std::max(
                1, slot.tiledEncoder->cols() * slot.tiledEncoder->rows());
        }
        slot.tiledEncoder->encode(px, static_cast<size_t>(w) * 4);
        return;
    }

    if (!slot.encoder) {
        slot.tileCount = 1;
        PlayerSlot* self = &slot;
        VideoEncoder::Config ec;
        ec.width = w;
        ec.height = h;
        ec.fps = 60;
        // Match tiled total budget when untiled (legacy path was 16 Mbps).
        ec.averageBitRate = 4'000'000;
        if (const char* e = std::getenv("GE_STREAM_BPS")) {
            int v = std::atoi(e);
            if (v > 0) ec.averageBitRate = v;
        }
        slot.encoder = std::make_unique<VideoEncoder>(
            ec, [this, self](VideoEncoder::Frame f) {
                onEncoded(*self, f);
            });
    }
    slot.encoder->encode(px, static_cast<size_t>(w) * 4);
}

void ServerSession::Impl::inputLoop(PlayerSlot* slot) {
    while (running.load() && slot->inputRunning.load() && slot->wire &&
           slot->wire->isOpen()) {
        std::vector<char> data;
        if (!slot->wire->recvBinary(data)) break;
        if (data.size() < sizeof(wire::MessageHeader)) continue;
        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), 4);
        if (magic == wire::kSdlEventMagic &&
            data.size() >= sizeof(wire::MessageHeader) + sizeof(SDL_Event)) {
            SDL_Event ev;
            std::memcpy(&ev, data.data() + sizeof(wire::MessageHeader),
                        sizeof(SDL_Event));
            std::lock_guard lock(slot->inputMu);
            slot->inputQ.push_back(ev);
        } else if (magic == wire::kDeviceInfoMagic &&
                   data.size() >=
                       sizeof(wire::MessageHeader) + sizeof(wire::DeviceInfo)) {
            wire::DeviceInfo info;
            std::memcpy(&info, data.data() + sizeof(wire::MessageHeader),
                        sizeof(info));
            SPDLOG_INFO(
                "ServerSession: sid={} DeviceInfo {}x{} @{}x class={} (hints)",
                slot->id, info.width, info.height, info.pixelRatio,
                info.deviceClass);
        }
    }
}

void ServerSession::Impl::openSlot(const std::string& sessionId) {
    {
        std::lock_guard lock(slotsMu);
        if (slots.count(sessionId)) return;
    }

    auto wire =
        connectWebSocket(host, port, "/ws/server/wire/" + sessionId, 2000);
    if (!wire || !wire->isOpen()) {
        SPDLOG_ERROR("ServerSession: wire open failed for session {}",
                     sessionId);
        return;
    }

    auto slot = std::make_shared<PlayerSlot>();
    slot->id = sessionId;
    slot->wire = std::move(wire);
    slot->encodeWin.windowStart = std::chrono::steady_clock::now();

    sendWire(*slot, wire::kSessionConfigMagic, &sessionConfig,
             sizeof(sessionConfig));
    sendWire(*slot, wire::kStreamSessionIdMagic, sessionId.data(),
             uint32_t(sessionId.size()));

    std::weak_ptr<PlayerSlot> weak = slot;
    slot->inputThread = std::thread([this, weak] {
        if (auto s = weak.lock()) inputLoop(s.get());
    });

    {
        std::lock_guard lock(slotsMu);
        slots[sessionId] = std::move(slot);
        anyPlayer.store(true);
    }
    {
        std::lock_guard lock(lifeMu);
        pendingAttach.push_back(sessionId);
    }
    SPDLOG_INFO("ServerSession: player attached (session {})", sessionId);
}

void ServerSession::Impl::closeSlot(const std::string& sessionId) {
    std::shared_ptr<PlayerSlot> slot;
    {
        std::lock_guard lock(slotsMu);
        auto it = slots.find(sessionId);
        if (it == slots.end()) return;
        slot = std::move(it->second);
        slots.erase(it);
        anyPlayer.store(!slots.empty());
    }
    if (!slot) return;

    slot->inputRunning.store(false);
    if (slot->wire) slot->wire->close();
    if (slot->inputThread.joinable()) slot->inputThread.join();
    if (slot->encoder) {
        slot->encoder->flush();
        slot->encoder.reset();
    }
    if (slot->tiledEncoder) {
        slot->tiledEncoder->flush();
        slot->tiledEncoder.reset();
    }
    {
        std::lock_guard lock(lifeMu);
        pendingDetach.push_back(sessionId);
    }
    SPDLOG_INFO("ServerSession: player detached (session {})", sessionId);
}

void ServerSession::Impl::sidebandLoop() {
    sideband = connectWebSocket(host, port, "/ws/server?name=" + name, 2000);
    if (!sideband || !sideband->isOpen()) {
        SPDLOG_WARN(
            "ServerSession: relay connect {}:{} failed — not streaming", host,
            port);
        return;
    }
    const std::string hello =
        "{\"type\":\"hello\",\"name\":\"" + name + "\",\"pid\":" +
        std::to_string(getpid()) +
        ",\"version\":" + std::to_string(wire::kProtocolVersion) + "}";
    sideband->sendText(hello);
    SPDLOG_INFO("ServerSession: connected to relay {}:{} as '{}'", host, port,
                name);

    while (running.load() && sideband->isOpen()) {
        std::vector<char> data;
        if (!sideband->recvBinary(data) || data.empty()) break;
        if (data[0] != '{') continue;
        const std::string msg(data.begin(), data.end());
        const std::string type = jsonStr(msg, "type");
        const std::string sessionId = jsonStr(msg, "session_id");
        if (type == "player_attached" && !sessionId.empty()) {
            openSlot(sessionId);
        } else if (type == "player_detached" && !sessionId.empty()) {
            closeSlot(sessionId);
        } else if (type == "player_detached") {
            // No id: tear down all (legacy).
            std::vector<std::string> ids;
            {
                std::lock_guard lock(slotsMu);
                for (auto& [id, _] : slots) ids.push_back(id);
            }
            for (const auto& id : ids) closeSlot(id);
        }
    }
    std::vector<std::string> ids;
    {
        std::lock_guard lock(slotsMu);
        for (auto& [id, _] : slots) ids.push_back(id);
    }
    for (const auto& id : ids) closeSlot(id);
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
    ge::appchannel::registerStateSlice(
        "stream",
        [this]() -> nlohmann::json {
            std::lock_guard lock(i_->slotsMu);
            nlohmann::json arr = nlohmann::json::array();
            for (auto& [id, slot] : i_->slots) {
                std::lock_guard sl(slot->statsMu);
                if (!slot->lastStreamStats.empty())
                    arr.push_back(slot->lastStreamStats);
                else
                    arr.push_back(nlohmann::json{
                        {"role", "server"},
                        {"name", i_->name},
                        {"sid", id},
                        {"on", true},
                    });
            }
            return nlohmann::json{
                {"role", "server"},
                {"name", i_->name},
                {"sessions", arr},
                {"n", arr.size()},
            };
        },
        nlohmann::json{{"role", "server"}, {"n", 0}});

    i_->running.store(true);
    i_->sidebandThread = std::thread([this] { i_->sidebandLoop(); });
}

void ServerSession::stop() {
    if (!i_->running.exchange(false)) return;
    if (i_->sideband) i_->sideband->close();
    std::vector<std::string> ids;
    {
        std::lock_guard lock(i_->slotsMu);
        for (auto& [id, _] : i_->slots) ids.push_back(id);
    }
    for (const auto& id : ids) i_->closeSlot(id);
    if (i_->sidebandThread.joinable()) i_->sidebandThread.join();
}

bool ServerSession::active() const { return i_->anyPlayer.load(); }

std::atomic<bool>* ServerSession::activeFlag() { return &i_->anyPlayer; }

void ServerSession::pollLifecycle(
    const std::function<void(const std::string&)>& onAttach,
    const std::function<void(const std::string&)>& onDetach) {
    std::vector<std::string> att, det;
    {
        std::lock_guard lock(i_->lifeMu);
        att.swap(i_->pendingAttach);
        det.swap(i_->pendingDetach);
    }
    for (const auto& id : att)
        if (onAttach) onAttach(id);
    for (const auto& id : det)
        if (onDetach) onDetach(id);
}

std::vector<std::string> ServerSession::sessionIds() const {
    std::lock_guard lock(i_->slotsMu);
    std::vector<std::string> ids;
    ids.reserve(i_->slots.size());
    for (auto& [id, _] : i_->slots) ids.push_back(id);
    return ids;
}

void ServerSession::drainInput(
    const std::string& sessionId,
    const std::function<void(const SDL_Event&)>& deliver) {
    std::vector<SDL_Event> batch;
    {
        std::lock_guard lock(i_->slotsMu);
        auto it = i_->slots.find(sessionId);
        if (it == i_->slots.end()) return;
        std::lock_guard il(it->second->inputMu);
        batch.swap(it->second->inputQ);
    }
    for (const auto& e : batch)
        if (deliver) deliver(e);
}

void ServerSession::setCaptureTarget(const std::string& sessionId) {
    std::lock_guard lock(i_->targetMu);
    i_->captureTarget = sessionId;
}

void ServerSession::onCapturedFrame(const std::uint8_t* px, int w, int h) {
    std::string target;
    {
        std::lock_guard lock(i_->targetMu);
        target = i_->captureTarget;
    }
    if (target.empty()) return;
    std::shared_ptr<PlayerSlot> slot;
    {
        std::lock_guard lock(i_->slotsMu);
        auto it = i_->slots.find(target);
        if (it == i_->slots.end()) return;
        slot = it->second;
    }
    if (slot) i_->encodeFrame(*slot, px, w, h);
}

} // namespace ge
