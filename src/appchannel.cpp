// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T92.1 — app side of spyder's bidirectional MessagePack-RPC channel.
// Transport + framing + hello handshake + request dispatch. Compiled out
// entirely under NDEBUG. See include/ge/appchannel.h for the wire format.

#include <ge/appchannel.h>

#include <spdlog/spdlog.h>

#ifndef NDEBUG

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include <SDL3/SDL.h>

#include "render/LifecycleInject.h"  // ge::detail::injectMemoryWarning

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

// Registered request handlers, keyed by method name. Populated before
// installFromEnv (single-threaded), read-only on the worker thread after.
std::unordered_map<std::string, Handler>& handlers() {
    static std::unordered_map<std::string, Handler> h;
    return h;
}

int dialTCP(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
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
        // Dev-only; detached — the OS reaps it on process exit.
        std::thread([this] { run(); }).detach();
    }

    bool active() const { return live_.load(); }

    void push(const std::string& method, nlohmann::json params) {
        if (!live_.load()) return;
        sendFrame(nlohmann::json{{"method", method}, {"params", std::move(params)}});
    }

    // Drain anything queued for the wire. Frames are sent synchronously under
    // sendMu_ today, so there is never a backlog and this returns immediately;
    // it exists so app_flush has a real drain point once T92.4 adds the
    // batched log/perf sender queue.
    void flush() {
        std::lock_guard<std::mutex> lk(sendMu_);
    }

private:
    void run() {
        fd_ = dialTCP(host_, port_);
        if (fd_ < 0) {
            SPDLOG_WARN("appchannel: connect to {}:{} failed", host_, port_);
            return;
        }
        // hello: advertise the methods we have handlers for.
        nlohmann::json methods = nlohmann::json::array();
        for (const auto& kv : handlers()) methods.push_back(kv.first);
        sendFrame(nlohmann::json{
            {"id", nextId_++},
            {"method", "hello"},
            {"params", {{"app_name", app_}, {"app_version", ver_}, {"methods", methods}}}});

        for (;;) {
            nlohmann::json msg;
            if (!recvFrame(msg)) break;
            dispatch(msg);
        }
        live_.store(false);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        SPDLOG_INFO("appchannel: channel closed");
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

    std::atomic<bool> started_{false};
    std::atomic<bool> live_{false};
    std::atomic<std::uint64_t> nextId_{1};
    std::string host_, app_, ver_;
    int port_ = 0;
    int fd_ = -1;
    std::mutex sendMu_;
};

// "appchannel://host:port" → {host, port}; port <= 0 if not that scheme.
std::pair<std::string, int> parseAppchannel(const std::string& target) {
    static constexpr char kScheme[] = "appchannel://";
    if (target.rfind(kScheme, 0) != 0) return {"", 0};
    const std::string hp = target.substr(sizeof(kScheme) - 1);
    const auto colon = hp.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= hp.size()) return {"", 0};
    int port = 0;
    try {
        port = std::stoi(hp.substr(colon + 1));
    } catch (...) {
        return {"", 0};
    }
    if (port <= 0 || port > 65535) return {"", 0};
    return {hp.substr(0, colon), port};
}

std::string resolveTarget() {
    if (const char* e = std::getenv("LOG_TARGET"); e && *e) return e;
#if defined(__ANDROID__)
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ge.log_target", buf) > 0 && buf[0]) return buf;
#endif
    return {};
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
}

} // namespace

void registerMethod(std::string method, Handler handler) {
    handlers()[std::move(method)] = std::move(handler);
}

void installFromEnv(const std::string& appName, const std::string& appVersion) {
    auto [host, port] = parseAppchannel(resolveTarget());
    if (port <= 0) return;
    registerBuiltins();
    Channel::instance().start(host, port, appName, appVersion);
    SPDLOG_INFO("appchannel: dialing appchannel://{}:{} (app={})", host, port, appName);
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

void push(std::string method, nlohmann::json params) {
    Channel::instance().push(method, std::move(params));
}

bool active() { return Channel::instance().active(); }

#else  // NDEBUG — feature compiled out entirely.

void registerMethod(std::string, Handler) {}
void installFromEnv(const std::string&, const std::string&) {}
void push(std::string, nlohmann::json) {}
bool active() { return false; }
float applyTimeControl(float realDt) { return realDt; }

#endif

} // namespace ge::appchannel
