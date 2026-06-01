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

#endif  // NDEBUG

namespace ge::appchannel {

#ifndef NDEBUG
namespace {

constexpr std::size_t kMaxBody = 16u * 1024 * 1024;  // 16 MB per spyder spec

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

} // namespace

void registerMethod(std::string method, Handler handler) {
    handlers()[std::move(method)] = std::move(handler);
}

void installFromEnv(const std::string& appName, const std::string& appVersion) {
    auto [host, port] = parseAppchannel(resolveTarget());
    if (port <= 0) return;
    // Built-in liveness probe so a freshly-connected channel is drivable.
    // Echoes the app's wall-clock millis so spyder's app_ping reports a real
    // round-trip timestamp (its documented "the timestamp the app saw").
    if (handlers().find("ping") == handlers().end()) {
        registerMethod("ping", [](const nlohmann::json&) {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            return nlohmann::json{{"ts", static_cast<std::int64_t>(ms)}};
        });
    }
    Channel::instance().start(host, port, appName, appVersion);
    SPDLOG_INFO("appchannel: dialing appchannel://{}:{} (app={})", host, port, appName);
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

#endif

} // namespace ge::appchannel
