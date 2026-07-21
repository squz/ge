// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T92.1 — app side of spyder's bidirectional MessagePack-RPC channel.
// Transport + framing + hello handshake + request dispatch. Compiled out
// entirely under NDEBUG. See include/ge/appchannel.h for the wire format.

#include <ge/appchannel.h>
#include <ge/metrics.h>
#include <ge/button.h>

#include <spdlog/spdlog.h>

#ifndef NDEBUG

#include <ge/Tweak.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>  // 🎯T92.6 IMG_SavePNG_IO

#include "appchannel_internal.h"      // ge::appchannel::detail::buildSliceDescriptors
#include "render/LifecycleInject.h"   // ge::detail::injectMemoryWarning
#include "render/SensorControl.h"     // fine-grained accel stream authority
#include "render/ScreenshotBridge.h"  // ge::detail::captureFrameRGBA

#endif  // NDEBUG

namespace ge::appchannel {

#ifndef NDEBUG
namespace {

constexpr std::size_t kMaxBody = 16u * 1024 * 1024;  // 16 MB per spyder spec

// 🎯T92.2 Dev time-control, driven by app_pause/resume/step/speed and read
// once per frame by the SessionHost run loop via applyTimeControl(). All
// reads/writes are atomic so the channel worker thread can mutate while the
// game thread reads. Identity values (not paused, 1× speed, no steps) make
// applyTimeControl a pass-through when nothing has driven them.
std::atomic<bool>  g_tcPaused{false};
std::atomic<float> g_tcSpeed{1.0f};
std::atomic<int>   g_tcStepFrames{0};
// A single stepped frame advances this much nominal time — fixed (not the
// real wall-clock dt, which would be huge after a pause) so frame-stepping is
// reproducible.
constexpr float kStepDt = 1.0f / 60.0f;

// 🎯T92.4 Perf counters. perfEmit (game thread) sets the latest value per
// name; perfTick accumulates frame time and, once the window elapses, pushes
// {frame_ms (window average), counters}.
std::mutex g_perfMu;
std::unordered_map<std::string, double> g_perfCounters;
float g_perfAccumMs = 0.0f;
int   g_perfFrames  = 0;
constexpr float kPerfWindowMs = 1000.0f;  // ~1 s cadence

// 🎯T92.5 State registry. Populated by registerStateSlice/Serializer BEFORE
// ge::run (single-threaded), read by the channel worker thread after — the
// happens-before is the channel-thread launch in start().
std::unordered_map<std::string, StateGetter> g_slices;
// 🎯T116 Optional per-slice example payloads, advertised in the hello as
// {name, example} descriptors. Same write-once-before-run lifetime as g_slices.
std::unordered_map<std::string, nlohmann::json> g_sliceExamples;
StateGetter   g_stateSaver;
StateRestorer g_stateRestorer;

// 🎯T109 Hit-target surface size (full-surface pts) + optional non-button extras.
float g_hitSurfaceW = 0.f;
float g_hitSurfaceH = 0.f;
nlohmann::json g_extraHitTargets = nlohmann::json::array();
bool g_hitTargetsSliceRegistered = false;

// Game-thread task queue. State handlers run on the worker thread but must
// observe game state from the game thread (no torn reads against the sim), so
// they marshal a task here and block on its result; pumpMainThreadTasks drains
// it from the run loop.
std::mutex g_taskMu;
std::deque<std::function<void()>> g_tasks;

// Marshal `fn` onto the game thread, block until it runs, return its result.
// `fn`'s exceptions propagate back to the caller (the dispatcher turns them
// into a JSON-RPC error). Caller's stack outlives the task (it blocks on the
// future), so the by-reference capture is safe.
nlohmann::json runOnGameThread(const std::function<nlohmann::json()>& fn) {
    std::promise<nlohmann::json> prom;
    auto fut = prom.get_future();
    {
        std::lock_guard<std::mutex> lk(g_taskMu);
        g_tasks.push_back([&] {
            try { prom.set_value(fn()); }
            catch (...) { prom.set_exception(std::current_exception()); }
        });
    }
    return fut.get();
}

// Registered request handlers, keyed by method name. Populated before
// installFromEnv (single-threaded), read-only on the worker thread after.
std::unordered_map<std::string, Handler>& handlers() {
    static std::unordered_map<std::string, Handler> h;
    return h;
}

// dialTCP attempts a single TCP connect to host:port. On failure the
// last errno is captured in *outErrno (when non-null) so callers can
// surface a useful diagnostic — the historical one-line
// "appchannel: connect to {}:{} failed" gave no signal whether the
// failure was ECONNREFUSED (listener down), EHOSTUNREACH (LAN
// unreachable / iOS local-network permission denial), ETIMEDOUT
// (network blip), or anything else.
int dialTCP(const std::string& host, int port, int* outErrno) {
    if (outErrno) *outErrno = 0;
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0) {
        // getaddrinfo doesn't set errno; surface the gai-specific code
        // in outErrno (negative to disambiguate from POSIX errno values).
        if (outErrno) *outErrno = -gai;
        return -1;
    }
    int fd = -1;
    int lastErr = 0;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { lastErr = errno; continue; }
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            lastErr = 0;
            break;
        }
        lastErr = errno;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && outErrno) *outErrno = lastErr;
    return fd;
}

class Channel {
public:
    static Channel& instance() {
        static Channel c;
        return c;
    }

    void start(std::string host, int port, std::string app, std::string ver) {
        if (started_.exchange(true)) return;
        host_ = std::move(host);
        port_ = port;
        app_  = std::move(app);
        ver_  = std::move(ver);
        // Dev-only; detached — the OS reaps both on process exit. The receiver
        // owns the socket reads + request dispatch; the sender drains queued
        // pushes so the game thread never blocks on the socket (🎯T92.1/T92.4).
        std::thread([this] { run(); }).detach();
        std::thread([this] { senderLoop(); }).detach();
    }

    bool active() const { return live_.load(); }

    // Enqueue an async push ({method, params}); never blocks on the socket.
    // Dropped if the channel isn't live, or if the backlog hits kMaxQueued
    // (oldest first) so a stalled listener can't grow memory without bound.
    void push(const std::string& method, nlohmann::json params) {
        if (!live_.load()) return;
        nlohmann::json env{{"method", method}, {"params", std::move(params)}};
        {
            std::lock_guard<std::mutex> lk(queueMu_);
            if (sendQueue_.size() >= kMaxQueued) sendQueue_.pop_front();
            sendQueue_.push_back(std::move(env));
        }
        queueCv_.notify_one();
    }

    // Block until the push queue is fully drained to the socket. Used by the
    // app_flush handler as a precondition for app_quit.
    void flush() {
        std::unique_lock<std::mutex> lk(queueMu_);
        drainedCv_.wait(lk, [this] { return sendQueue_.empty(); });
        lk.unlock();
        // The queue is empty, but the sender may still be mid-write on the last
        // frame; grabbing sendMu_ waits for that write to finish.
        std::lock_guard<std::mutex> sl(sendMu_);
    }

    // 🎯T136 Bounded flush for the crash path: drain within `timeoutMs` and
    // return, never blocking indefinitely (a last-gasp reporter must not hang).
    void flushFor(int timeoutMs) {
        std::unique_lock<std::mutex> lk(queueMu_);
        drainedCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [this] { return sendQueue_.empty(); });
    }

private:
    // Bounded retry across the dial — the first attempt can lose to a
    // race (spyder listener up but routed late after device wake), or
    // to the iOS local-network permission prompt (silent EHOSTUNREACH
    // until the user taps Allow). Schedule: 0/200/700/1700/3700/8700ms,
    // total ~9s. errno is logged per attempt so the failure mode is
    // diagnosable instead of "connect failed" with no signal.
    void run() {
        static constexpr int kBackoffsMs[] = {200, 500, 1000, 2000, 5000};
        constexpr int kAttempts = sizeof(kBackoffsMs) / sizeof(kBackoffsMs[0]) + 1;
        int lastErr = 0;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kBackoffsMs[attempt - 1]));
            }
            fd_ = dialTCP(host_, port_, &lastErr);
            if (fd_ >= 0) break;
            const char* reason = lastErr < 0
                ? gai_strerror(-lastErr)
                : std::strerror(lastErr);
            SPDLOG_WARN("appchannel: connect to {}:{} failed (attempt {}/{}, errno={}: {})",
                        host_, port_, attempt + 1, kAttempts, lastErr, reason);
        }
        if (fd_ < 0) {
            const char* reason = lastErr < 0
                ? gai_strerror(-lastErr)
                : std::strerror(lastErr);
            SPDLOG_WARN(
                "appchannel: giving up after {} attempts (last errno={}: {}). "
                "If this is iOS, check that NSLocalNetworkUsageDescription is "
                "declared in Info.plist and the local-network permission was "
                "granted; otherwise check that spyder is up and the listener "
                "address matches SPYDER_APP_CHANNEL.",
                kAttempts, lastErr, reason);
            stopSender();
            return;
        }
        // hello: advertise the methods we handle and the state slices
        // consumers registered (before ge::run).
        nlohmann::json methods = nlohmann::json::array();
        for (const auto& kv : handlers()) methods.push_back(kv.first);
        // 🎯T116 Build mixed slice descriptors: bare name, or {name, example}
        // for slices that volunteered one. buildSliceDescriptors is pure (unit-
        // tested in appchannel_test.cpp against spyder's SliceDescriptor forms).
        std::vector<std::pair<std::string, nlohmann::json>> sliceList;
        sliceList.reserve(g_slices.size());
        for (const auto& kv : g_slices) {
            auto ex = g_sliceExamples.find(kv.first);
            sliceList.emplace_back(
                kv.first,
                ex != g_sliceExamples.end() ? ex->second : nlohmann::json(nullptr));
        }
        nlohmann::json slices = detail::buildSliceDescriptors(sliceList);
        sendFrame(nlohmann::json{
            {"id", nextId_++},
            {"method", "hello"},
            {"params", {{"app_name", app_}, {"app_version", ver_},
                        {"methods", methods}, {"slices", slices}}}});

        for (;;) {
            nlohmann::json msg;
            if (!recvFrame(msg)) break;
            dispatch(msg);
        }
        live_.store(false);
        stopSender();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        SPDLOG_INFO("appchannel: channel closed");
    }

    // Sender thread: drain queued pushes onto the wire. Frame writes share
    // sendMu_ with the receiver thread's request responses.
    void senderLoop() {
        for (;;) {
            nlohmann::json env;
            {
                std::unique_lock<std::mutex> lk(queueMu_);
                queueCv_.wait(lk, [this] { return senderStop_ || !sendQueue_.empty(); });
                if (senderStop_ && sendQueue_.empty()) return;
                env = std::move(sendQueue_.front());
                sendQueue_.pop_front();
            }
            sendFrame(env);  // holds sendMu_ for the actual write
            {
                std::lock_guard<std::mutex> lk(queueMu_);
                if (sendQueue_.empty()) drainedCv_.notify_all();
            }
        }
    }

    void stopSender() {
        {
            std::lock_guard<std::mutex> lk(queueMu_);
            senderStop_ = true;
        }
        queueCv_.notify_all();
        drainedCv_.notify_all();
    }

    void dispatch(const nlohmann::json& msg) {
        // hello response → channel goes live.
        if (!live_.load() && msg.contains("result") && msg["result"].is_object()
            && msg["result"].contains("spyder_version")) {
            live_.store(true);
            SPDLOG_INFO("appchannel: handshake ok (spyder {})",
                        msg["result"].value("spyder_version", std::string{"?"}));
            return;
        }
        // request from spyder → dispatch + respond.
        if (msg.contains("id") && msg.contains("method")) {
            const std::string method = msg["method"];
            const nlohmann::json params =
                msg.contains("params") ? msg["params"] : nlohmann::json::object();
            nlohmann::json resp{{"id", msg["id"]}};
            auto it = handlers().find(method);
            if (it == handlers().end()) {
                resp["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
            } else {
                try {
                    resp["result"] = it->second(params);
                } catch (const Error& e) {
                    resp["error"] = {{"code", e.code}, {"message", e.message}};
                } catch (const std::exception& e) {
                    resp["error"] = {{"code", -32000}, {"message", e.what()}};
                }
            }
            sendFrame(resp);
        }
    }

    void sendFrame(const nlohmann::json& env) {
        std::vector<std::uint8_t> body = nlohmann::json::to_msgpack(env);
        if (body.size() > kMaxBody) return;
        const std::uint32_t len = static_cast<std::uint32_t>(body.size());
        const std::uint8_t hdr[4] = {
            static_cast<std::uint8_t>(len & 0xff),
            static_cast<std::uint8_t>((len >> 8) & 0xff),
            static_cast<std::uint8_t>((len >> 16) & 0xff),
            static_cast<std::uint8_t>((len >> 24) & 0xff)};
        std::lock_guard<std::mutex> lk(sendMu_);
        if (fd_ < 0) return;
        if (writeAll(hdr, 4)) writeAll(body.data(), body.size());
    }

    bool recvFrame(nlohmann::json& out) {
        std::uint8_t hdr[4];
        if (!readAll(hdr, 4)) return false;
        const std::uint32_t len = std::uint32_t(hdr[0]) | (std::uint32_t(hdr[1]) << 8)
                                | (std::uint32_t(hdr[2]) << 16) | (std::uint32_t(hdr[3]) << 24);
        if (len == 0 || len > kMaxBody) return false;
        std::vector<std::uint8_t> body(len);
        if (!readAll(body.data(), len)) return false;
        try {
            out = nlohmann::json::from_msgpack(body);
        } catch (const std::exception& e) {
            SPDLOG_WARN("appchannel: msgpack decode failed: {}", e.what());
            return false;
        }
        return true;
    }

    bool writeAll(const std::uint8_t* p, std::size_t n) {
        while (n) {
            ssize_t w = ::send(fd_, p, n, 0);
            if (w <= 0) return false;
            p += w;
            n -= static_cast<std::size_t>(w);
        }
        return true;
    }
    bool readAll(std::uint8_t* p, std::size_t n) {
        while (n) {
            ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<std::size_t>(r);
        }
        return true;
    }

    static constexpr std::size_t kMaxQueued = 4096;

    std::atomic<bool> started_{false};
    std::atomic<bool> live_{false};
    std::atomic<std::uint64_t> nextId_{1};
    std::string host_, app_, ver_;
    int port_ = 0;
    int fd_ = -1;
    std::mutex sendMu_;

    // Async push queue, drained by senderLoop().
    std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::condition_variable drainedCv_;
    std::deque<nlohmann::json> sendQueue_;
    bool senderStop_ = false;
};

// "host:port" → {host, port}; port <= 0 on any parse failure. Splits on the
// last colon so bare IPv4 / hostnames work (bracketed IPv6 isn't supported —
// dev-only, localhost / LAN is the use case).
std::pair<std::string, int> parseTarget(const std::string& target) {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) return {"", 0};
    int port = 0;
    try {
        port = std::stoi(target.substr(colon + 1));
    } catch (...) {
        return {"", 0};
    }
    if (port <= 0 || port > 65535) return {"", 0};
    return {target.substr(0, colon), port};
}

// 🎯T119 Spyder's app-channel address: SPYDER_APP_CHANNEL=host:port. spyder's
// launch_app / deploy_app inject it; on Android ge.GeActivity lifts the launch
// Intent's string-extras into the process env via setenv before the native
// thread starts, so getenv() works the same on every platform.
std::string resolveTarget() {
    if (const char* e = std::getenv("SPYDER_APP_CHANNEL"); e && *e) return e;
    return {};
}

// Coerce a JSON field to float, tolerating spyder's RPC passthrough
// delivering extra (non-schema) args as strings ("0.14") rather than JSON
// numbers. Returns dflt when the field is absent or unparseable.
float jnum(const nlohmann::json& p, const char* key, float dflt) {
    if (!p.is_object() || !p.contains(key)) return dflt;
    const auto& v = p[key];
    if (v.is_number()) return v.get<float>();
    if (v.is_string()) {
        try { return std::stof(v.get<std::string>()); }
        catch (...) { return dflt; }
    }
    return dflt;
}

// Push an SDL event of just a type onto SDL's (thread-safe) event queue, so
// it surfaces in the consumer's next pumpEvents indistinguishably from a real
// OS event. Used for the lifecycle injections that map cleanly to SDL events.
void pushSdlType(Uint32 type) {
    SDL_Event e{};
    e.type = type;
    SDL_PushEvent(&e);
}

// Register ge-owned method handlers. Idempotent and consumer-friendly: a
// method already registered (e.g. by a consumer overriding "ping", or a
// re-entry) is left untouched.
void registerBuiltins() {
    auto reg = [](const char* name, Handler h) {
        if (handlers().find(name) == handlers().end())
            handlers().emplace(name, std::move(h));
    };
    const nlohmann::json kAck = nlohmann::json::object();

    // ── liveness ──
    // Echo the app's wall-clock millis so spyder's app_ping reports a real
    // round-trip timestamp (its documented "the timestamp the app saw").
    reg("ping", [](const nlohmann::json&) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return nlohmann::json{{"ts", static_cast<std::int64_t>(ms)}};
    });

    // ── tweaks (🎯T91.2) ──
    // Expose the shared `tweak::` library over the app-channel so a
    // direct-mode app is tunable from spyder WITHOUT a separate broker daemon — this is what
    // lets the control plane live in spyder rather than a ge-side daemon. list/get
    // use the exact `tweak::allToJson` serialisation spyder app-channel proxies, so
    // the shapes match; set/reset apply + persist through the tweak DB.
    reg("tweak_list", [](const nlohmann::json&) {
        return nlohmann::json::parse(tweak::allToJson());
    });
    reg("tweak_get", [](const nlohmann::json& p) -> nlohmann::json {
        if (!p.contains("name"))
            throw Error{-32602, "tweak_get: 'name' is required"};
        const std::string name = p["name"].get<std::string>();
        for (auto& t : nlohmann::json::parse(tweak::allToJson()))
            if (t.value("name", std::string{}) == name) return t;
        throw Error{-32602, "tweak_get: no such tweak: " + name};
    });
    reg("tweak_set", [](const nlohmann::json& p) -> nlohmann::json {
        if (!p.contains("name") || !p.contains("value"))
            throw Error{-32602, "tweak_set: 'name' and 'value' are required"};
        if (!tweak::parseAndApply(p.dump()))
            throw Error{-32602, "tweak_set: no such tweak or invalid value: " +
                                    p.value("name", std::string{})};
        // Return the updated entry (JSON) — richer than a bare text ack; the
        // harness records this as an intentional divergence (Goodhart guard).
        const std::string name = p["name"].get<std::string>();
        for (auto& t : nlohmann::json::parse(tweak::allToJson()))
            if (t.value("name", std::string{}) == name) return t;
        return nlohmann::json::object();
    });
    reg("tweak_reset", [](const nlohmann::json& p) -> nlohmann::json {
        // {"name":..} resets one, {"all":true} resets all — same payload the old broker
        // forwards, applied locally via the shared library. Returns the fresh
        // list so the caller sees the result in one round-trip.
        tweak::parseAndReset(p.dump());
        return nlohmann::json::parse(tweak::allToJson());
    });

    // ── lifecycle ──
    // quit: enqueue SDL_QUIT → the run loop exits via shouldQuit(), onShutdown
    // runs, process exits 0 (no macOS crash notification). Acked before the
    // main thread processes the event, so spyder sees the ack.
    reg("quit", [kAck](const nlohmann::json&) {
        pushSdlType(SDL_EVENT_QUIT);
        return kAck;
    });
    // flush: drain the wire sender, then ack (precondition for app_quit).
    reg("flush", [kAck](const nlohmann::json&) {
        Channel::instance().flush();
        return kAck;
    });
    // backgrounded / foregrounded: post the SDL lifecycle events the engine
    // already acts on (🎯T7/T88 audio + render gating).
    reg("backgrounded", [kAck](const nlohmann::json&) {
        pushSdlType(SDL_EVENT_DID_ENTER_BACKGROUND);
        return kAck;
    });
    reg("foregrounded", [kAck](const nlohmann::json&) {
        pushSdlType(SDL_EVENT_DID_ENTER_FOREGROUND);
        return kAck;
    });
    // low_memory_warning: routed through the engine's pending-warning atomic
    // (see render/LifecycleInject.h) rather than an SDL event, to avoid double-
    // firing the iOS observer. iOS reports a single ungraded warning → Critical.
    reg("low_memory_warning", [kAck](const nlohmann::json&) {
        ge::detail::injectMemoryWarning(MemoryPressureLevel::Critical);
        return kAck;
    });

    // ── time-control ── (read by the run loop via applyTimeControl)
    reg("pause", [kAck](const nlohmann::json&) {
        g_tcStepFrames.store(0);
        g_tcPaused.store(true);
        return kAck;
    });
    reg("resume", [kAck](const nlohmann::json&) {
        g_tcStepFrames.store(0);
        g_tcSpeed.store(1.0f);      // "resume normal pacing" — clear any speed
        g_tcPaused.store(false);
        return kAck;
    });
    reg("step", [kAck](const nlohmann::json& p) {
        int frames = (p.is_object() && p.contains("frames")) ? p.value("frames", 1) : 1;
        if (frames < 1) frames = 1;
        g_tcStepFrames.store(frames);
        g_tcPaused.store(true);     // step implies "advance N then re-pause"
        return kAck;
    });
    reg("speed", [kAck](const nlohmann::json& p) {
        float m = (p.is_object() && p.contains("multiplier")) ? p.value("multiplier", 1.0f) : 1.0f;
        if (m < 0.0f) m = 0.0f;
        g_tcSpeed.store(m);         // orthogonal to pause; inert while paused
        return kAck;
    });

    // ── input injection (🎯T92.3) ──
    // Fabricate an SDL_Event and SDL_PushEvent it. SDL's event queue is
    // thread-safe, so the event surfaces in the consumer's onEvent on the
    // next pumpEvents indistinguishably from a real OS event — including the
    // engine's own transforms (sensor → screen-frame rotation, refresh-rate
    // press tracking). Touch coords are normalized 0–1 exactly as SDL
    // delivers real touches, so ge::input::fromSdl denormalizes them against
    // the surface size like any real finger.
    reg("input_inject", [kAck](const nlohmann::json& p) {
        if (!p.is_object() || !p.contains("type"))
            throw Error{-32602, "input_inject: missing 'type'"};
        const std::string type = p.value("type", std::string{});
        SDL_Event e{};
        if (type == "finger_down" || type == "finger_up" || type == "finger_motion") {
            e.type = (type == "finger_down")  ? SDL_EVENT_FINGER_DOWN
                   : (type == "finger_up")    ? SDL_EVENT_FINGER_UP
                                              : SDL_EVENT_FINGER_MOTION;
            e.tfinger.touchID  = static_cast<SDL_TouchID>(0x1ABC);  // synthetic, non-zero
            e.tfinger.fingerID = static_cast<SDL_FingerID>(jnum(p, "id", 1.0f));
            e.tfinger.x        = jnum(p, "x", 0.0f);
            e.tfinger.y        = jnum(p, "y", 0.0f);
            e.tfinger.dx       = jnum(p, "dx", 0.0f);
            e.tfinger.dy       = jnum(p, "dy", 0.0f);
            e.tfinger.pressure = jnum(p, "pressure", 1.0f);
        } else if (type == "key_down" || type == "key_up") {
            const std::string key = p.value("key", std::string{});
            const SDL_Keycode kc = SDL_GetKeyFromName(key.c_str());
            if (kc == SDLK_UNKNOWN)
                throw Error{-32602, "input_inject: unknown key '" + key + "'"};
            e.type         = (type == "key_down") ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.scancode = SDL_GetScancodeFromKey(kc, nullptr);
            e.key.key      = kc;
            e.key.mod      = SDL_KMOD_NONE;
            e.key.down     = (type == "key_down");
            e.key.repeat   = false;
        } else if (type == "accel") {
            // Device-frame acceleration; engine rotates to screen frame in
            // pumpEvents. Prefer value=[x,y,z]; x/y/z still accepted.
            float ax = 0.f, ay = 0.f, az = 0.f;
            if (p.contains("value") && p["value"].is_array() && p["value"].size() >= 3) {
                ax = p["value"][0].get<float>();
                ay = p["value"][1].get<float>();
                az = p["value"][2].get<float>();
            } else {
                ax = jnum(p, "x", 0.0f);
                ay = jnum(p, "y", 0.0f);
                az = jnum(p, "z", 0.0f);
            }
            e.type           = SDL_EVENT_SENSOR_UPDATE;
            e.sensor.which   = static_cast<SDL_SensorID>(ge::detail::kSyntheticAccelWhich);
            e.sensor.data[0] = ax;
            e.sensor.data[1] = ay;
            e.sensor.data[2] = az;
            // While suppressed: update latch so pumpEvents re-asserts each frame.
            if (ge::detail::accelStreamMode() == ge::detail::SensorStreamMode::Override) {
                ge::detail::setAccelLatch(ax, ay, az);
            }
        } else {
            throw Error{-32602, "input_inject: unknown type '" + type + "'"};
        }
        SDL_PushEvent(&e);
        return kAck;
    });

    // ── sensor stream authority (one concern per method; sticky suppress) ──
    //   sensor_suppress   {sensor?}              — drop real samples (sticky)
    //   sensor_set        {sensor?, value=[x,y,z]} — set scripted sample (while suppressed)
    //   sensor_unsuppress {sensor?}              — restore real device stream
    //   sensor_status     {sensor?}              — query {suppressed, value?}
    // Only "accel" today. Touch/keys never affected. suppress does NOT set a value.
    auto requireAccel = [](const nlohmann::json& p) {
        const std::string sensor = p.value("sensor", std::string{"accel"});
        if (sensor != "accel")
            throw Error{-32602, "unknown sensor '" + sensor + "' (supported: accel)"};
        return sensor;
    };
    auto parseValue3 = [](const nlohmann::json& p, float& x, float& y, float& z) {
        if (!p.contains("value") || !p["value"].is_array() || p["value"].size() < 3)
            throw Error{-32602, "value must be a 3-number array [x,y,z]"};
        x = p["value"][0].get<float>();
        y = p["value"][1].get<float>();
        z = p["value"][2].get<float>();
    };
    auto statusJson = []() -> nlohmann::json {
        using M = ge::detail::SensorStreamMode;
        const auto m = ge::detail::accelStreamMode();
        nlohmann::json out{{"sensor", "accel"},
                           {"suppressed", m != M::Passthrough}};
        float x, y, z;
        if (m == M::Mute) {
            out["value"] = nlohmann::json::array({0.0, 0.0, 0.0});
        } else if (ge::detail::accelLatch(x, y, z)) {
            out["value"] = nlohmann::json::array({x, y, z});
        }
        return out;
    };

    reg("sensor_suppress", [requireAccel, statusJson](const nlohmann::json& p) {
        requireAccel(p);
        // Sticky until sensor_unsuppress. Does not set a sample.
        ge::detail::setAccelStreamMode(ge::detail::SensorStreamMode::Override);
        return statusJson();
    });
    reg("sensor_set", [requireAccel, parseValue3, statusJson](const nlohmann::json& p) {
        requireAccel(p);
        if (ge::detail::accelStreamMode() == ge::detail::SensorStreamMode::Passthrough)
            throw Error{-32602, "sensor_set: sensor is not suppressed "
                                "(call sensor_suppress first)"};
        float x, y, z;
        parseValue3(p, x, y, z);
        ge::detail::setAccelLatch(x, y, z);
        // Ensure we re-assert latch (not mute/neutral-only).
        ge::detail::setAccelStreamMode(ge::detail::SensorStreamMode::Override);
        return statusJson();
    });
    reg("sensor_unsuppress", [requireAccel, statusJson](const nlohmann::json& p) {
        requireAccel(p);
        ge::detail::setAccelStreamMode(ge::detail::SensorStreamMode::Passthrough);
        return statusJson();
    });
    reg("sensor_status", [requireAccel, statusJson](const nlohmann::json& p) {
        requireAccel(p);
        return statusJson();
    });

    // ── per-instance metrics ring (🎯T166) ──
    auto resolveMetricsScope = [](const nlohmann::json& p) -> ge::metrics::Scope* {
        if (p.contains("instance") && p["instance"].is_string()) {
            auto* s = ge::metrics::Scope::find(p["instance"].get<std::string>());
            if (!s)
                throw Error{-32602, "metrics: unknown instance"};
            return s;
        }
        auto all = ge::metrics::Scope::all();
        if (all.empty())
            throw Error{-32601, "metrics: no Scope registered (create ge::metrics::Scope per instance)"};
        if (all.size() > 1)
            throw Error{-32602, "metrics: multiple instances; pass 'instance' id"};
        return all[0];
    };
    reg("metrics_list", [resolveMetricsScope](const nlohmann::json& p) {
        // With no instance and multiple scopes, list all instances' catalogs.
        if (!p.contains("instance") && ge::metrics::Scope::all().size() != 1) {
            nlohmann::json out = nlohmann::json::array();
            for (auto* s : ge::metrics::Scope::all())
                out.push_back(s->list());
            return nlohmann::json{{"instances", std::move(out)}};
        }
        return resolveMetricsScope(p)->list();
    });
    reg("metrics_arm", [resolveMetricsScope](const nlohmann::json& p) {
        auto* s = resolveMetricsScope(p);
        if (!p.contains("series") || !p["series"].is_array())
            throw Error{-32602, "metrics_arm: 'series' array required"};
        std::vector<std::string> series;
        for (const auto& el : p["series"]) {
            if (el.is_string()) series.push_back(el.get<std::string>());
        }
        std::size_t cap = 3600;
        if (p.contains("capacity")) {
            if (p["capacity"].is_number_unsigned())
                cap = p["capacity"].get<std::size_t>();
            else if (p["capacity"].is_number_integer())
                cap = (std::size_t)std::max(0, p["capacity"].get<int>());
        }
        s->arm(series, cap);
        return s->status();
    });
    reg("metrics_disarm", [resolveMetricsScope](const nlohmann::json& p) {
        auto* s = resolveMetricsScope(p);
        s->disarm();
        return s->status();
    });
    reg("metrics_status", [resolveMetricsScope](const nlohmann::json& p) {
        return resolveMetricsScope(p)->status();
    });
    reg("metrics_dump", [resolveMetricsScope](const nlohmann::json& p) {
        return resolveMetricsScope(p)->dump();
    });

    // ── state registry (🎯T92.5) ── (getters marshalled to the game thread)
    reg("state_query", [](const nlohmann::json& p) {
        const std::string slice = p.value("slice", std::string{});
        auto it = g_slices.find(slice);
        if (it == g_slices.end())
            throw Error{-32602, "state_query: unknown slice '" + slice + "'"};
        return runOnGameThread(it->second);
    });
    reg("save_state", [](const nlohmann::json&) {
        if (!g_stateSaver)
            throw Error{-32601, "save_state: no serializer registered"};
        const nlohmann::json snap = runOnGameThread(g_stateSaver);
        // Return the snapshot as raw MessagePack bytes in a `state` bin field;
        // spyder base64-encodes it into the app_save_state {state_b64, size}.
        const std::vector<std::uint8_t> bytes = nlohmann::json::to_msgpack(snap);
        return nlohmann::json{{"state", nlohmann::json::binary(bytes)}};
    });
    reg("restore_state", [kAck](const nlohmann::json& p) {
        if (!g_stateRestorer)
            throw Error{-32601, "restore_state: no serializer registered"};
        // spyder base64-decodes the tool's state_b64 and hands us the raw
        // bytes back as the `state` bin.
        if (!p.is_object() || !p.contains("state") || !p["state"].is_binary())
            throw Error{-32602, "restore_state: missing 'state' bin"};
        const auto& bytes = p["state"].get_binary();
        nlohmann::json snap;
        try { snap = nlohmann::json::from_msgpack(bytes); }
        catch (const std::exception& e) {
            throw Error{-32602, std::string("restore_state: bad blob: ") + e.what()};
        }
        runOnGameThread([snap] { g_stateRestorer(snap); return nlohmann::json(nullptr); });
        return kAck;
    });

    // ── screenshot (🎯T92.6) ──
    // Capture the framebuffer (game thread, via the render host), PNG-encode
    // it here (off the render thread), and return {format, width, height,
    // data:<bin>} — the PNG rides as a MessagePack bin, no base64.
    reg("screenshot_app", [](const nlohmann::json&) {
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
        if (!ge::detail::captureFrameRGBA(rgba, w, h) || w <= 0 || h <= 0)
            throw Error{-32000, "screenshot_app: capture failed (timed out or not rendering)"};

        SDL_Surface* surf =
            SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
        if (!surf)
            throw Error{-32000, std::string("screenshot_app: surface: ") + SDL_GetError()};

        std::vector<std::uint8_t> png;
        if (SDL_IOStream* io = SDL_IOFromDynamicMem()) {
            if (IMG_SavePNG_IO(surf, io, /*closeio=*/false)) {
                const Sint64 n = SDL_TellIO(io);
                void* ptr = SDL_GetPointerProperty(
                    SDL_GetIOProperties(io),
                    SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr);
                if (ptr && n > 0)
                    png.assign(static_cast<std::uint8_t*>(ptr),
                               static_cast<std::uint8_t*>(ptr) + n);
            }
            SDL_CloseIO(io);
        }
        SDL_DestroySurface(surf);
        if (png.empty())
            throw Error{-32000, "screenshot_app: PNG encode failed"};

        return nlohmann::json{
            {"format", "png"},
            {"width",  w},
            {"height", h},
            {"data",   nlohmann::json::binary(std::move(png))},
        };
    });
}

} // namespace

void registerMethod(std::string method, Handler handler) {
    handlers()[std::move(method)] = std::move(handler);
}

// Defined with the hit-target registry helpers below (must precede installFromEnv use).
void ensureHitTargetsSliceRegistered();

void installFromEnv(const std::string& appName, const std::string& appVersion) {
    auto [host, port] = parseTarget(resolveTarget());
    if (port <= 0) return;
    // 🎯T109 Advertise hit_targets in hello even before any Button publishes.
    ensureHitTargetsSliceRegistered();
    registerBuiltins();
    Channel::instance().start(host, port, appName, appVersion);
    SPDLOG_INFO("appchannel: dialing spyder app-channel {}:{} (app={})", host, port, appName);
}

float applyTimeControl(float realDt) {
    if (g_tcPaused.load()) {
        if (g_tcStepFrames.load() > 0) {
            g_tcStepFrames.fetch_sub(1);
            return kStepDt;     // advance exactly one frame; re-holds at 0
        }
        return 0.0f;            // held: render + input continue, no sim advance
    }
    return realDt * g_tcSpeed.load();
}

void perfEmit(const std::string& name, double value) {
    std::lock_guard<std::mutex> lk(g_perfMu);
    g_perfCounters[name] = value;
}

void perfTick(float frameMs) {
    if (!Channel::instance().active()) return;  // nothing to push to
    // spyder's app_perf_get reads {timestamp, samples:{name→value}}; frame_ms
    // rides as just another sample alongside the consumer's perfEmit counters.
    nlohmann::json samples = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lk(g_perfMu);
        g_perfAccumMs += frameMs;
        g_perfFrames  += 1;
        if (g_perfAccumMs < kPerfWindowMs) return;  // still inside the window
        samples["frame_ms"] = g_perfAccumMs / float(g_perfFrames);
        for (const auto& kv : g_perfCounters) samples[kv.first] = kv.second;
        g_perfAccumMs = 0.0f;
        g_perfFrames  = 0;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Channel::instance().push("perf", nlohmann::json{
        {"ts",      static_cast<std::int64_t>(ms)},  // 🎯T119 spyder PerfPush is msgpack:"ts"
        {"samples", std::move(samples)},
    });
}

namespace detail {

nlohmann::json buildSliceDescriptors(
    const std::vector<std::pair<std::string, nlohmann::json>>& slices) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [name, example] : slices) {
        if (example.is_null())
            out.push_back(name);                                    // bare string
        else
            out.push_back({{"name", name}, {"example", example}});  // {name, example}
    }
    return out;
}

nlohmann::json buildHitTargetsPayload(
    float surfaceW, float surfaceH,
    const std::vector<HitTargetExport>& items,
    const nlohmann::json& extras) {
    nlohmann::json targets = nlohmann::json::array();
    const bool haveSurface = surfaceW > 0.f && surfaceH > 0.f;
    auto appendOne = [&](const HitTargetExport& it) {
        if (it.id.empty() || it.w == 0.f || it.h == 0.f) return;
        nlohmann::json t{
            {"id", it.id},
            {"kind", it.kind.empty() ? "button" : it.kind},
            {"enabled", it.enabled},
            {"space", "pts"},
            {"bbox", {it.x, it.y, it.w, it.h}},
        };
        if (!it.role.empty()) t["role"] = it.role;
        if (!it.label.empty()) t["label"] = it.label;
        if (haveSurface) {
            t["bbox_norm"] = {it.x / surfaceW, it.y / surfaceH,
                              it.w / surfaceW, it.h / surfaceH};
            t["center_norm"] = {(it.x + 0.5f * it.w) / surfaceW,
                                (it.y + 0.5f * it.h) / surfaceH};
        }
        targets.push_back(std::move(t));
    };
    for (const auto& it : items) appendOne(it);
    if (extras.is_array()) {
        for (const auto& e : extras) {
            if (!e.is_object()) continue;
            HitTargetExport it;
            it.id      = e.value("id", "");
            it.kind    = e.value("kind", "region");
            it.role    = e.value("role", "");
            it.label   = e.value("label", "");
            it.enabled = e.value("enabled", true);
            if (e.contains("bbox") && e["bbox"].is_array() && e["bbox"].size() >= 4) {
                it.x = e["bbox"][0].get<float>();
                it.y = e["bbox"][1].get<float>();
                it.w = e["bbox"][2].get<float>();
                it.h = e["bbox"][3].get<float>();
            }
            // If caller already supplied norm fields only, pass through later.
            if (it.w == 0.f && e.contains("center_norm")) {
                // Still require bbox for contract; skip incomplete extras.
                continue;
            }
            appendOne(it);
            // Preserve precomputed center_norm/bbox_norm if present on extra.
            if (!targets.empty() && e.contains("center_norm")) {
                targets.back()["center_norm"] = e["center_norm"];
            }
            if (!targets.empty() && e.contains("bbox_norm")) {
                targets.back()["bbox_norm"] = e["bbox_norm"];
            }
        }
    }
    return nlohmann::json{{"targets", std::move(targets)}};
}

} // namespace detail

namespace {

nlohmann::json collectPublishedHitTargets() {
    std::vector<detail::HitTargetExport> items;
    for (Button* b : publishedHitTargets()) {
        if (!b || b->id.empty() || b->hitBounds.empty()) continue;
        detail::HitTargetExport it;
        it.id      = b->id;
        it.kind    = "button";
        it.role    = b->role;
        it.label   = b->label;
        it.enabled = b->hitEnabled;
        it.x = b->hitBounds.x;
        it.y = b->hitBounds.y;
        it.w = b->hitBounds.w;
        it.h = b->hitBounds.h;
        items.push_back(std::move(it));
    }
    return detail::buildHitTargetsPayload(
        g_hitSurfaceW, g_hitSurfaceH, items, g_extraHitTargets);
}

} // namespace

void ensureHitTargetsSliceRegistered() {
    if (g_hitTargetsSliceRegistered) return;
    g_hitTargetsSliceRegistered = true;
    registerStateSlice(
        "hit_targets",
        [] { return collectPublishedHitTargets(); },
        nlohmann::json{
            {"targets", nlohmann::json::array({
                {{"id", "example"},
                 {"kind", "button"},
                 {"role", "reset"},
                 {"label", "Example"},
                 {"enabled", true},
                 {"space", "pts"},
                 {"bbox", {100.0, 8.0, 200.0, 40.0}},
                 {"bbox_norm", {0.1, 0.01, 0.2, 0.05}},
                 {"center_norm", {0.2, 0.035}}},
            })},
        });
}

void registerStateSlice(std::string name, StateGetter getter, nlohmann::json example) {
    if (!example.is_null()) g_sliceExamples[name] = std::move(example);
    g_slices[std::move(name)] = std::move(getter);
}

void registerStateSerializer(StateGetter save, StateRestorer restore) {
    g_stateSaver    = std::move(save);
    g_stateRestorer = std::move(restore);
}

void pumpMainThreadTasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lk(g_taskMu);
            if (g_tasks.empty()) break;
            task = std::move(g_tasks.front());
            g_tasks.pop_front();
        }
        task();
    }
}

void push(std::string method, nlohmann::json params) {
    Channel::instance().push(method, std::move(params));
}

bool active() { return Channel::instance().active(); }

void flush(int timeoutMs) { Channel::instance().flushFor(timeoutMs); }

void setHitTargetSurfacePts(float width, float height) {
    g_hitSurfaceW = width;
    g_hitSurfaceH = height;
}

void setExtraHitTargets(nlohmann::json targetsArray) {
    if (targetsArray.is_null()) {
        g_extraHitTargets = nlohmann::json::array();
        return;
    }
    if (!targetsArray.is_array()) return;
    g_extraHitTargets = std::move(targetsArray);
}

#else  // NDEBUG — feature compiled out entirely.

void registerMethod(std::string, Handler) {}
void installFromEnv(const std::string&, const std::string&) {}
void push(std::string, nlohmann::json) {}
bool active() { return false; }
void flush(int) {}
float applyTimeControl(float realDt) { return realDt; }
void perfEmit(const std::string&, double) {}
void perfTick(float) {}
void registerStateSlice(std::string, StateGetter, nlohmann::json) {}
void registerStateSerializer(StateGetter, StateRestorer) {}
void pumpMainThreadTasks() {}
void setHitTargetSurfacePts(float, float) {}
void setExtraHitTargets(nlohmann::json) {}

#endif

} // namespace ge::appchannel
