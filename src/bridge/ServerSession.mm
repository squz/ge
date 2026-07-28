// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "ServerSession.h"

#include "../render/LifecycleInject.h"

#include <ge/CmdStream.h>
#include <ge/Protocol.h>
#include <ge/SeatPolicy.h>
#include <ge/StreamHostPolicy.h>
#include <ge/VideoEncoder.h>
#include <ge/ViewerMetrics.h>
#include <ge/WebSocketClient.h>
#include <ge/WireSdlEvent.h>

#include <ge/audio.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <unistd.h>  // getpid

#include <algorithm>
#include <chrono>
#include <map>

#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>

extern char** environ;
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
    std::uint32_t hostSessionId = 0;  // T175.8 bound via setSessionId
    std::string host;
    int port = 0;
    std::string name;
    wire::SessionConfig sessionConfig{};

    std::shared_ptr<WsConnection> sideband;
    // Multi-session (🎯T100.2 / T154): one wire per attached player.
    // `wire` remains the most recently opened connection for code paths that
    // still assume a single peer (sqlpipe push, input loop primary).
    std::shared_ptr<WsConnection> wire;
    std::vector<std::shared_ptr<WsConnection>> wires;
    std::mutex wireSendMu;

    std::unique_ptr<VideoEncoder> encoder;
    int encW = 0, encH = 0;
    std::atomic<uint32_t> seq{0};

    std::atomic<bool> running{false};
    std::atomic<bool> hasPlayer{false};
    std::thread sidebandThread;
    // One input reader per player wire (blocking recv).
    std::vector<std::thread> inputThreads;
    std::mutex inputDispatchMu; // DeviceInfo / SP2T / negotiate shared state
    // 🎯T156.3: primary seat = first attach; spectators get content only.
    SeatPolicy seats;
    // 🎯T159: seq allocated at onFrameBegin, stamped into SP2F at onFrameEnd.
    uint32_t lastFrameSeq = 0;
    // 🎯T156.3 per-session detach: relay names the detached session; close
    // only that wire (guarded by wireSendMu).
    std::map<std::string, std::weak_ptr<WsConnection>> wireBySession;
    void closeWireForSession(const std::string& sessionId);
    // 🎯T156.3 late joiners: a wire attaching mid-stream has an empty blob
    // cache, so warm frames reference textures it never received (bytes
    // flow, glass stays black). Each attach forces one cold frame — the
    // cache clear happens on the RENDER thread at the next onFrameBegin.
    std::atomic<bool> forceColdStart{false};
    // 🎯T163 per-session instances: default multi-session model. The
    // registered process is a MOTHER (catalogue presence + supervisor);
    // each player_attached spawns a CHILD process (GE_WIRE_SESSION env)
    // that opens that one session's wire and runs its own complete game —
    // world, Context, SP2T seeded from its own glass, capture, transport.
    // GE_BROADCAST=1 opts in to the legacy shared-world fan-out for
    // genuinely shared-world titles.
    bool childMode = false;
    std::string childSession;
    bool broadcastMode = false;
    void spawnChild(const std::string& sessionId);

    // Player surface discovery (DeviceInfo / SafeAreaUpdate). Applied to game
    // Context by DirectRenderHost when streaming. Encode/cmdstream size remains
    // the server surface — never resized from DeviceInfo.
    ViewerMetricsStore viewer{};

    // 🎯T154 SP2T: host Context::db() working set (:memory: under stream).
    // Player dumps seed on attach; continuous maybePushSp2t + detach push
    // keep the player durable file current.
    std::shared_ptr<sqlpipe::Database> workingDb;
    std::mutex workingDbMu;
    std::string schemaDdl;  // re-applied after player seed (sqlift-safe tables)
    std::chrono::steady_clock::time_point lastSp2tPush{};
    static constexpr auto kSp2tPushInterval =
        std::chrono::milliseconds(500);

    // 🎯T128.9 — negotiated transport. Server offers H.264 + cmdstream;
    // pick the highest rung in the intersection with player DeviceInfo
    // capabilities (cmdstream > H.264). TRANSPORT=h264 forces baseline for
    // debugging. SessionConfig is re-sent after negotiation.
    std::atomic<uint8_t> transport{wire::kTransportH264};
    cmdstream::Cache cmdCache; // server-side "already sent" memo for SP2S
    cmdstream::LiveCapture live; // sprite-run capture under cmdstream
    // false when cmdstream is selected — skip multi-MB GPU readback.
    std::atomic<bool> capturePixels{true};
    // Legacy Present path (not used for 60 fps goal).

    void sidebandLoop();
    void openWire(const std::string& sessionId);
    void closeWire();
    void wireInputLoop(std::shared_ptr<WsConnection> w);
    void dispatchPlayerMessage(const std::vector<char>& data, SeatId from);
    void onEncoded(VideoEncoder::Frame f);  // VideoToolbox thread
    void negotiateTransport(uint8_t playerCaps);

    void sendWire(uint32_t magic, const void* payload, uint32_t payloadLen) {
        wire::MessageHeader hdr{magic, payloadLen};
        std::vector<uint8_t> msg(sizeof(hdr) + payloadLen);
        std::memcpy(msg.data(), &hdr, sizeof(hdr));
        if (payloadLen) std::memcpy(msg.data() + sizeof(hdr), payload, payloadLen);
        std::lock_guard<std::mutex> lk(wireSendMu);
        // Fan-out to every open player wire (multi-session).
        for (auto& w : wires) {
            if (w && w->isOpen()) w->sendBinary(msg.data(), msg.size());
        }
        if (wires.empty() && wire && wire->isOpen()) {
            wire->sendBinary(msg.data(), msg.size());
        }
    }

    // Dump working set and send SP2T. Caller holds no workingDbMu.
    // Returns true if a non-empty snapshot was sent.
    bool pushSp2tSnapshot(const char* reason) {
        std::vector<uint8_t> dump;
        {
            std::lock_guard<std::mutex> lk(workingDbMu);
            if (!workingDb || !wire || !wire->isOpen()) return false;
            if (!dumpSqliteMain(workingDb->handle(), dump) || dump.empty())
                return false;
        }
        sendWire(wire::kSqlpipeMsgMagic, dump.data(),
                 static_cast<uint32_t>(dump.size()));
        SPDLOG_INFO("ServerSession: SP2T snapshot pushed ({} bytes, {})",
                    dump.size(), reason);
        return true;
    }
};

void ServerSession::Impl::onEncoded(VideoEncoder::Frame f) {
    if (!f.data || f.size == 0) return;
    uint32_t s = seq.fetch_add(1);
    uint8_t flags = f.isKeyframe ? 1 : 0;
    uint32_t payloadSize = uint32_t(sizeof(flags) + sizeof(s) + f.size);
    WireWriter wr(sizeof(wire::MessageHeader) + payloadSize);
    wr.put(wire::MessageHeader{wire::kVideoStreamMagic, payloadSize});
    wr.put(flags);
    wr.put(s);
    wr.append(f.data, f.size);
    std::lock_guard<std::mutex> lk(wireSendMu);
    bool any = false;
    for (auto& w : wires) {
        if (w && w->isOpen()) {
            w->sendBinary(wr.data(), wr.size());
            any = true;
        }
    }
    if (!any && wire && wire->isOpen()) wire->sendBinary(wr.data(), wr.size());
}

void ServerSession::Impl::openWire(const std::string& sessionId) {
    auto w = connectWebSocket(host, port, "/ws/server/wire/" + sessionId, 2000);
    if (!w || !w->isOpen()) {
        SPDLOG_ERROR("ServerSession: wire open failed for session {}", sessionId);
        return;
    }
    const bool first = !hasPlayer.load();
    {
        std::lock_guard<std::mutex> lk(wireSendMu);
        wires.push_back(w);
        wireBySession[sessionId] = w;
        wire = w; // latest peer for single-wire helpers
    }
    hasPlayer.store(true);

    // Advertise session requirements BEFORE the player creates its window —
    // the player blocks on this in PlayerWireBridge::connect (handshake step 1).
    // Send SessionConfig only on this new wire (not fan-out to existing players).
    {
        wire::MessageHeader hdr{wire::kSessionConfigMagic,
                                static_cast<uint32_t>(sizeof(sessionConfig))};
        std::vector<uint8_t> msg(sizeof(hdr) + sizeof(sessionConfig));
        std::memcpy(msg.data(), &hdr, sizeof(hdr));
        std::memcpy(msg.data() + sizeof(hdr), &sessionConfig, sizeof(sessionConfig));
        std::lock_guard<std::mutex> lk(wireSendMu);
        w->sendBinary(msg.data(), msg.size());
    }
    SPDLOG_INFO("ServerSession: SessionConfig sensors={:#x} orientation={} "
                "transport={} flags={:#x} (immersive={} noSaver={}) session={}",
                sessionConfig.sensors, sessionConfig.orientation,
                sessionConfig.transport, sessionConfig.flags,
                (sessionConfig.flags & wire::kSessionFlagImmersive) ? 1 : 0,
                (sessionConfig.flags & wire::kSessionFlagNoScreenSaver) ? 1 : 0,
                sessionId);

    // Primary seat = first attach (🎯T156.3). Spectators still get content
    // fan-out; only primary may drive DeviceInfo and game input/sensors.
    {
        std::lock_guard<std::mutex> lk(inputDispatchMu);
        seats.onAttach(w.get());
    }
    (void)first;
    forceColdStart.store(true); // late joiner → next frame is cold for all
    inputThreads.emplace_back([this, w] { wireInputLoop(w); });
    SPDLOG_INFO("ServerSession: player attached (session {}), wires={} primary={}",
                sessionId, wires.size(), seats.isPrimary(w.get()) ? 1 : 0);
}

void ServerSession::Impl::closeWire() {
    if (!hasPlayer.exchange(false)) return;
    // 🎯T154 SP2T: final push of working set to player durable store.
    pushSp2tSnapshot("detach");
    if (encoder) {
        encoder->flush();
        encoder.reset();
    }
    encW = encH = 0;
    transport.store(wire::kTransportH264);
    capturePixels.store(true);
    // Disarm + destroy Writer under LiveCapture's mutex so the render
    // thread cannot UAF emitBlob mid-frame (was SIGSEGV on attach).
    cmdstream::setLiveCapture(hostSessionId, nullptr);  // T175.6
    live.resetSession();
    cmdCache.clear();
    {
        std::lock_guard<std::mutex> lk(wireSendMu);
        for (auto& w : wires) {
            if (w) w->close();
        }
        wires.clear();
        if (wire) wire->close();
        wire.reset();
    }
    for (auto& t : inputThreads) {
        if (t.joinable()) t.join();
    }
    inputThreads.clear();
    seats.onDetachAll();
    SPDLOG_INFO("ServerSession: player detached (all sessions)");
}

// 🎯T158: SP2A to the primary seat only — spectators must not toggle
// relative mouse. Lock order matches dispatch: inputDispatchMu, wireSendMu.
void ServerSession::sendArmState(bool armed) {
    wire::ArmState as{};
    as.armed = armed ? 1 : 0;
    wire::MessageHeader hdr{wire::kArmStateMagic,
                            static_cast<uint32_t>(sizeof(as))};
    std::vector<uint8_t> msg(sizeof(hdr) + sizeof(as));
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    std::memcpy(msg.data() + sizeof(hdr), &as, sizeof(as));
    std::lock_guard<std::mutex> lk(i_->inputDispatchMu);
    const SeatId prim = i_->seats.primary();
    if (prim == nullptr) return;
    std::lock_guard<std::mutex> slk(i_->wireSendMu);
    for (auto& w : i_->wires) {
        if (w && w.get() == prim && w->isOpen()) {
            w->sendBinary(msg.data(), msg.size());
            SPDLOG_INFO("ServerSession: SP2A arm={} → primary seat", as.armed);
            break;
        }
    }
}

void ServerSession::Impl::spawnChild(const std::string& sessionId) {
    char exe[4096];
    uint32_t sz = sizeof(exe);
    if (_NSGetExecutablePath(exe, &sz) != 0) {
        SPDLOG_ERROR("ServerSession: executable path too long — cannot "
                     "spawn child for session {}", sessionId);
        return;
    }
    std::vector<std::string> envStrings;
    for (char** e = environ; *e; ++e) {
        // Child gets a fresh trace target; drop the mother's, if any.
        if (std::strncmp(*e, "GE_SENSOR_TRACE=", 16) == 0) continue;
        envStrings.emplace_back(*e);
    }
    envStrings.push_back("GE_WIRE_SESSION=" + sessionId);
    if (const char* dir = std::getenv("GE_CHILD_TRACE_DIR")) {
        envStrings.push_back(std::string("GE_SENSOR_TRACE=") + dir + "/" +
                             sessionId + ".trace");
    }
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (auto& e : envStrings) envp.push_back(e.data());
    envp.push_back(nullptr);
    char* argv[] = {exe, nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe, nullptr, nullptr, argv, envp.data());
    if (rc != 0) {
        SPDLOG_ERROR("ServerSession: child spawn failed for session {}: {}",
                     sessionId, rc);
        return;
    }
    SPDLOG_INFO("ServerSession: spawned child pid {} for session {} "
                "(own world/state)", pid, sessionId);
}

void ServerSession::Impl::closeWireForSession(const std::string& sessionId) {
    std::shared_ptr<WsConnection> victim;
    {
        std::lock_guard<std::mutex> lk(wireSendMu);
        auto it = wireBySession.find(sessionId);
        if (it != wireBySession.end()) {
            victim = it->second.lock();
            wireBySession.erase(it);
        }
    }
    if (victim) {
        SPDLOG_INFO("ServerSession: closing wire for detached session {}",
                    sessionId);
        victim->close(); // recv loop exits → cull + seat promotion
    } else {
        // Already gone: the wire's own death usually beats the relay's
        // sideband notification here (the wire thread culls + promotes
        // first). This MUST be a no-op — falling back to full teardown
        // nuked every attached glass whenever one detached (observed
        // 2026-07-19: seven sessions killed in 25 ms by one terminate).
        SPDLOG_INFO("ServerSession: detach for already-culled session {} — "
                    "no-op", sessionId);
    }
}

void ServerSession::Impl::wireInputLoop(std::shared_ptr<WsConnection> w) {
    const SeatId from = w.get();
    while (running.load() && w && w->isOpen()) {
        std::vector<char> data;
        if (!w->recvBinary(data)) break;
        std::lock_guard<std::mutex> lk(inputDispatchMu);
        dispatchPlayerMessage(data, from);
    }
    if (!running.load()) return; // session teardown handles the rest

    // Per-wire death (🎯T156.3): cull this wire; if it held the primary
    // seat, promote the eldest survivor and re-establish its authority.
    // Lock order matches dispatch: inputDispatchMu, then wireSendMu.
    std::shared_ptr<WsConnection> promoted;
    {
        std::lock_guard<std::mutex> lk(inputDispatchMu);
        const bool primaryLost = seats.onDetach(from);
        std::shared_ptr<WsConnection> survivor;
        bool lastWireGone = false;
        {
            std::lock_guard<std::mutex> sendLk(wireSendMu);
            wires.erase(std::remove_if(wires.begin(), wires.end(),
                                       [&](const std::shared_ptr<WsConnection>& x) {
                                           return x.get() == w.get();
                                       }),
                        wires.end());
            for (auto it = wireBySession.begin(); it != wireBySession.end();) {
                auto sp = it->second.lock();
                if (!sp || sp.get() == w.get()) it = wireBySession.erase(it);
                else ++it;
            }
            if (primaryLost && !wires.empty()) survivor = wires.front();
            lastWireGone = wires.empty();
        }
        if (lastWireGone && childMode) {
            SPDLOG_INFO("ServerSession: session {} wire closed — child "
                        "exiting", childSession);
            std::exit(0);
        }
        if (lastWireGone) {
            // Last glass gone: reset stream state. Mirrors closeWire minus
            // socket/thread handling — this IS an input thread, so joining
            // inputThreads here would deadlock; stop()/closeWire still reap.
            hasPlayer.store(false);
            if (encoder) {
                encoder->flush();
                encoder.reset();
            }
            encW = encH = 0;
            transport.store(wire::kTransportH264);
            capturePixels.store(true);
            cmdstream::setLiveCapture(hostSessionId, nullptr);  // T175.6
            live.resetSession();
            cmdCache.clear();
            SPDLOG_INFO("ServerSession: last wire gone — stream state reset");
        }
        if (primaryLost) {
            // The stored viewer surface/authority belonged to the departed
            // seat. Invalidate; the promoted seat's DeviceInfo (sent in
            // response to SessionConfig below) re-establishes it.
            viewer.valid.store(false, std::memory_order_release);
            viewer.hasAccel.store(false, std::memory_order_relaxed);
            if (survivor) {
                seats.promote(survivor.get());
                promoted = survivor;
            }
            SPDLOG_INFO("ServerSession: primary seat detached; {}",
                        promoted ? "promoted eldest survivor" : "no seats left");
        }
    }
    if (promoted) {
        wire::MessageHeader hdr{wire::kSessionConfigMagic,
                                static_cast<uint32_t>(sizeof(sessionConfig))};
        std::vector<uint8_t> msg(sizeof(hdr) + sizeof(sessionConfig));
        std::memcpy(msg.data(), &hdr, sizeof(hdr));
        std::memcpy(msg.data() + sizeof(hdr), &sessionConfig,
                    sizeof(sessionConfig));
        std::lock_guard<std::mutex> lk(wireSendMu);
        promoted->sendBinary(msg.data(), msg.size());
    }
}

void ServerSession::Impl::dispatchPlayerMessage(const std::vector<char>& data,
                                                SeatId from) {
    if (data.size() < sizeof(wire::MessageHeader)) return;
    uint32_t magic = 0;
    std::memcpy(&magic, data.data(), 4);
    if (magic == wire::kSdlEventMagic) {
        SDL_Event ev{};
        // Shipped unpack (same as loopback inject oracle).
        if (!wire::unpackSdlEvent(
                reinterpret_cast<const uint8_t*>(data.data()), data.size(), ev))
            return;
        if (!seats.acceptSdlEvent(from, ev)) return; // spectator
        SDL_PushEvent(&ev);
    } else if (magic == wire::kDeviceInfoMagic &&
               data.size() >= sizeof(wire::MessageHeader) + 14) {
        if (!seats.acceptDeviceInfo(from)) {
            SPDLOG_INFO("ServerSession: ignoring DeviceInfo from non-primary seat");
            return;
        }
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
                               info.drawSafeW, info.drawSafeH,
                               (info.capabilities & wire::kCapHasAccelerometer) != 0);
        SPDLOG_INFO("ServerSession: player DeviceInfo {}x{} @{}x class={} "
                    "ui=({},{} {}x{}) draw=({},{} {}x{}) caps={:#x}",
                    info.width, info.height, info.pixelRatio, info.deviceClass,
                    info.safeX, info.safeY, info.safeW, info.safeH,
                    info.drawSafeX, info.drawSafeY, info.drawSafeW, info.drawSafeH,
                    info.capabilities);
        negotiateTransport(info.capabilities);
    } else if (magic == wire::kSafeAreaMagic &&
               data.size() >= sizeof(wire::MessageHeader) + 12) {
        if (!seats.acceptSafeArea(from)) return;
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
            detail::injectViewerBackgrounded(hostSessionId, false);  // T175.8
            ge::audio::onForeground();
            break;
        case wire::kLifeBackground:
            detail::injectViewerBackgrounded(hostSessionId, true);  // T175.8
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
        // 🎯T154 SP2T: player → server durable seed into :memory: working set.
        const auto* payload = reinterpret_cast<const uint8_t*>(
            data.data() + sizeof(wire::MessageHeader));
        const size_t n = data.size() - sizeof(wire::MessageHeader);
        std::lock_guard<std::mutex> lk(workingDbMu);
        if (workingDb && n > 0 &&
            loadSqliteMain(workingDb->handle(),
                           std::span<const uint8_t>(payload, n))) {
            // Player only seeds when it has user tables, so schema travels
            // with the dump. schemaDdl was applied at Database open before
            // this replace; notify watchers of the new contents.
            workingDb->notify();
            SPDLOG_INFO("ServerSession: SP2T snapshot applied ({} bytes, "
                        "schemaDdl={}B retained for bind)",
                        n, schemaDdl.size());
        } else {
            SPDLOG_WARN("ServerSession: SP2T snapshot apply failed "
                        "(db={} n={})", workingDb ? 1 : 0, n);
        }
    }
}

void ServerSession::Impl::negotiateTransport(uint8_t playerCaps) {
    // Rung ladder (high → low): cmdstream, H.264. Server always offers both;
    // player advertises via DeviceInfo.capabilities. Pick the best common
    // rung — no env opt-in required to "fall up".
    const bool playerCmd = (playerCaps & wire::kCapCommandStream) != 0;
    const char* pref = std::getenv("TRANSPORT");
    // Optional force-down for debugging (TRANSPORT=h264). TRANSPORT=cmdstream
    // is accepted as a no-op synonym for auto when the player can replay.
    const bool forceH264 =
        pref && (std::strcmp(pref, "h264") == 0 || std::strcmp(pref, "H264") == 0);

    uint8_t next = wire::kTransportH264;
    if (playerCmd && !forceH264) {
        next = wire::kTransportCommandStream;
    }

    // Idempotent: broadcast the update only when the rung actually changes.
    // The player answers any SessionConfig with DeviceInfo (connect contract,
    // also used for seat promotion), so an unconditional send here livelocks
    // the handshake: DeviceInfo → SessionConfig → DeviceInfo → … (observed
    // 6k+ rounds — black glass).
    const uint8_t prev = transport.load();
    transport.store(next);
    sessionConfig.transport = next;
    if (next == prev) return;
    sendWire(wire::kSessionConfigMagic, &sessionConfig, sizeof(sessionConfig));
    SPDLOG_INFO("ServerSession: transport={} (player_cmdstream={} force_h264={})",
                next == wire::kTransportCommandStream ? "cmdstream" : "h264",
                playerCmd, forceH264);

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
        // Multi-session (🎯T100.2): every player_attached opens a wire.
        // Detach currently tears down all sessions (fan-out encode model).
        if (type == "player_attached" && !sessionId.empty()) {
            if (broadcastMode) {
                openWire(sessionId); // explicit opt-in: shared-world fan-out
            } else {
                spawnChild(sessionId); // 🎯T163: per-session game instance
            }
        } else if (type == "player_detached") {
            // 🎯T156.3: the relay names the session (relay.go sends
            // session_id); close only that wire so surviving seats keep
            // their sessions and promotion can actually run. No id →
            // legacy full teardown. In per-session-instance mode the
            // children own their lifecycle (their wire dies → they exit).
            if (!broadcastMode) {
                SPDLOG_INFO("ServerSession: detach {} — child owns teardown",
                            sessionId);
            } else if (!sessionId.empty()) {
                closeWireForSession(sessionId);
            } else {
                closeWire();
            }
        }
    }
    closeWire();
    // 🎯T100.1 replace, process side: the relay closes this sideband when a
    // newer same-name server registers (or the relay itself goes away). A
    // superseded console host must EXIT, not linger — orphans keep ticking
    // physics and rendering headless at 60 fps; ten of them accumulated in
    // one dev day and starved the live server (visible as frame stutter on
    // the glass). closeWire() above already pushed the final SP2T snapshot.
    if (running.load()) {
        SPDLOG_INFO("ServerSession: sideband closed (replaced or relay gone) "
                    "— exiting superseded server process");
        std::exit(0);
    }
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

void ServerSession::setSessionId(std::uint32_t sid) { i_->hostSessionId = sid; }

void ServerSession::start() {
    i_->running.store(true);
    if (const char* cs = std::getenv("GE_WIRE_SESSION"); cs && cs[0]) {
        // 🎯T163 child: no catalogue registration, no sideband — open this
        // one session's wire and run a complete independent game for it.
        i_->childMode = true;
        i_->childSession = cs;
        i_->sidebandThread = std::thread([this] {
            i_->openWire(i_->childSession);
            if (!i_->hasPlayer.load()) {
                SPDLOG_ERROR("ServerSession: child wire open failed for "
                             "session {} — exiting", i_->childSession);
                std::exit(1);
            }
        });
        return;
    }
    if (const char* b = std::getenv("GE_BROADCAST"); b && b[0] == '1') {
        i_->broadcastMode = true;
        SPDLOG_INFO("ServerSession: BROADCAST mode (opt-in shared world)");
    }
    // Mother: auto-reap spawned children.
    signal(SIGCHLD, SIG_IGN);
    i_->sidebandThread = std::thread([this] { i_->sidebandLoop(); });
}

void ServerSession::stop() {
    if (!i_->running.exchange(false)) return;
    if (i_->sideband) i_->sideband->close();
    {
        std::lock_guard<std::mutex> lk(i_->wireSendMu);
        for (auto& w : i_->wires) {
            if (w) w->close();
        }
        if (i_->wire) i_->wire->close();
    }
    if (i_->sidebandThread.joinable()) i_->sidebandThread.join();
    for (auto& t : i_->inputThreads) {
        if (t.joinable()) t.join();
    }
    i_->inputThreads.clear();
}

bool ServerSession::active() const { return i_->hasPlayer.load(); }

std::atomic<bool>* ServerSession::activeFlag() { return &i_->hasPlayer; }

ViewerMetricsStore* ServerSession::viewerMetrics() { return &i_->viewer; }

void ServerSession::setWorkingDb(std::shared_ptr<sqlpipe::Database> db,
                                 std::string schemaDdl) {
    std::lock_guard<std::mutex> lk(i_->workingDbMu);
    i_->workingDb = std::move(db);
    i_->schemaDdl = std::move(schemaDdl);
    SPDLOG_INFO("ServerSession: SP2T working db bound ({}, schema={} bytes)",
                i_->workingDb ? "ok" : "null", i_->schemaDdl.size());
}

void ServerSession::maybePushSp2t() {
    if (!i_->hasPlayer.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (i_->lastSp2tPush.time_since_epoch().count() != 0 &&
        now - i_->lastSp2tPush < Impl::kSp2tPushInterval) {
        return;
    }
    // Mark interval even if dump is empty so we do not spin dump every frame
    // when the working set is missing.
    i_->lastSp2tPush = now;
    i_->pushSp2tSnapshot("stream");
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
    i_->lastFrameSeq = s;
    if (i_->forceColdStart.exchange(false)) {
        // Render thread: safe to reset cmdstream state. Existing glasses
        // tolerate the re-sent full blobs; the newcomer needs them.
        i_->cmdCache.clear();
        i_->live.resetSession();
        SPDLOG_INFO("ServerSession: cold frame forced for late joiner");
    }
    const uint16_t cw = contentW > 0 ? static_cast<uint16_t>(
        std::min(contentW, 65535)) : 0;
    const uint16_t ch = contentH > 0 ? static_cast<uint16_t>(
        std::min(contentH, 65535)) : 0;
    i_->live.begin(s, &i_->cmdCache, cw, ch);
    cmdstream::setLiveCapture(i_->hostSessionId, &i_->live);  // T175.6
}

void ServerSession::onFrameEnd() {
    if (cmdstream::liveCapture() != &i_->live) return;
    cmdstream::setLiveCapture(i_->hostSessionId, nullptr);  // T175.6
    auto payload = i_->live.end();
    if (payload.empty()) return;
    const auto st = i_->live.stats();
    // 🎯T159: emit metadata precedes its frame on the same ordered stream.
    {
        using namespace std::chrono;
        wire::FrameMeta fm{};
        fm.seq = i_->lastFrameSeq;
        fm.serverUs = static_cast<uint64_t>(duration_cast<microseconds>(
            system_clock::now().time_since_epoch()).count());
        i_->sendWire(wire::kFrameMetaMagic, &fm,
                     static_cast<uint32_t>(sizeof(fm)));
    }
    i_->sendWire(wire::kCommandStreamMagic, payload.data(),
                 static_cast<uint32_t>(payload.size()));
    // Bandwidth telemetry: every frame when CMDSTREAM_BW_LOG=1, else
    // first 5 frames + every 60th (steady-state sample).
    static uint32_t logN = 0;
    const uint32_t n = ++logN;
    static const bool every =
        [] {
            const char* e = std::getenv("CMDSTREAM_BW_LOG");
            return e && e[0] == '1';
        }();
    if (every || n <= 5 || (n % 60) == 0) {
        SPDLOG_INFO("ServerSession: SP2S frame#{} wire={}B runs={} "
                    "full_blobs={} refs={} full_blob_bytes={}",
                    n, payload.size(), i_->live.runCount(),
                    st.fullBlobCount, st.refBlobCount, st.fullBlobBytes);
    }
}

} // namespace ge
