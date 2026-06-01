// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::log::install dispatcher.
// Platform-specific sinks live in:
//   * log_apple.mm    — os_log sink (iOS / iPadOS / tvOS / watchOS).
//   * log_android.cpp — logcat sink (Android).
// macOS desktop and other platforms get the spdlog default colour
// stderr sink (left untouched).

#include <ge/log.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 🎯T83 Dev-only TCP log sink. The whole feature is compiled out of
// release builds (NDEBUG) so a misconfigured store binary can never open
// a socket to a developer's LAN. See NetworkLogSink below.
#ifndef NDEBUG
#include <spdlog/sinks/base_sink.h>
#ifdef _WIN32
#include <spdlog/details/tcp_client-windows.h>
#else
#include <spdlog/details/tcp_client.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <thread>
#include <utility>

#if defined(__ANDROID__)
#include <sys/system_properties.h>  // __system_property_get
#endif
#endif  // NDEBUG

namespace ge::log {

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)
// Defined in log_apple.mm.
spdlog::sink_ptr makeAppleSink(const std::string& subsystem);
std::string autoDetectSubsystemApple();
#endif

#if defined(__ANDROID__)
// Defined in log_android.cpp.
spdlog::sink_ptr makeAndroidSink(const std::string& subsystem);
std::string autoDetectSubsystemAndroid();
#endif

#ifndef NDEBUG
namespace {

// Resolve the dev-log target string. Primary source is the
// LOG_TARGET environment variable (set via Xcode scheme on iOS,
// shell env on desktop, or spyder's launch env-passthrough). Android
// apps aren't shell-launched, so env vars don't reach them; there we
// also accept the `debug.ge.log_target` system property, settable with
// `adb shell setprop debug.ge.log_target <host>:<port>` — no Java /
// Intent plumbing required.
std::string resolveLogTarget() {
    if (const char* env = std::getenv("LOG_TARGET"); env && *env)
        return env;
#if defined(__ANDROID__)
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ge.log_target", buf) > 0 && buf[0])
        return buf;
#endif
    return {};
}

// Parse "host:port". Splits on the last colon so bare IPv4 / hostnames
// work; bracketed IPv6 isn't supported (dev-only, localhost / LAN is the
// use case). Returns port <= 0 on any parse failure.
std::pair<std::string, int> parseTarget(const std::string& s) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
        return {"", 0};
    int port = 0;
    try {
        port = std::stoi(s.substr(colon + 1));
    } catch (...) {
        return {"", 0};
    }
    if (port <= 0 || port > 65535) return {"", 0};
    return {s.substr(0, colon), port};
}

// 🎯T83 Dev-only TCP sink: mirrors every spdlog emission to a host:port
// so a developer can tail logs with `nc -l <port>`, bypassing Apple
// unified logging (DTX/RSD-tunnel fragility) and Android logcat entirely.
//
// Why not spdlog's stock tcp_sink: it attempts a *blocking* reconnect on
// the calling thread for every log line while the listener is down, and
// throws on each failure. ge logs from the game thread, so that would
// hitch rendering and spam the error handler. This sink formats on the
// caller, hands the line to a dedicated sender thread via a bounded
// queue, and reconnects with exponential backoff — a downed listener
// costs nothing on the hot path, and a flood of logs can't grow memory
// without bound (oldest lines are dropped past kMaxQueued).
class NetworkLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    NetworkLogSink(std::string host, int port)
        : host_(std::move(host)), port_(port), worker_([this] { run(); }) {}

    ~NetworkLogSink() override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            stop_ = true;
        }
        qcv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        {
            std::lock_guard<std::mutex> lk(qmu_);
            if (queue_.size() >= kMaxQueued) queue_.pop_front();  // drop oldest
            queue_.emplace_back(buf.data(), buf.size());
        }
        qcv_.notify_one();
    }

    void flush_() override {}

private:
    static constexpr std::size_t kMaxQueued = 4096;
    static constexpr int kConnectTimeoutMs = 200;
    static constexpr auto kBackoffMin = std::chrono::milliseconds(250);
    static constexpr auto kBackoffMax = std::chrono::milliseconds(5000);

    void run() {
        spdlog::details::tcp_client client;
        auto backoff = kBackoffMin;
        auto nextRetry = std::chrono::steady_clock::now();
        for (;;) {
            std::deque<std::string> batch;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                qcv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                batch.swap(queue_);
            }
            for (auto& line : batch) {
                if (!client.is_connected()) {
                    if (std::chrono::steady_clock::now() < nextRetry)
                        continue;  // in a backoff window — drop, retry later
                    try {
                        client.connect(host_, port_, kConnectTimeoutMs);
                        backoff = kBackoffMin;  // reset on success
                    } catch (const std::exception&) {
                        nextRetry = std::chrono::steady_clock::now() + backoff;
                        backoff = std::min(backoff * 2, kBackoffMax);
                        continue;
                    }
                }
                try {
                    client.send(line.data(), line.size());
                } catch (const std::exception&) {
                    client.close();
                    nextRetry = std::chrono::steady_clock::now() + backoff;
                    backoff = std::min(backoff * 2, kBackoffMax);
                }
            }
        }
    }

    std::string host_;
    int port_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::string> queue_;
    bool stop_ = false;
    std::thread worker_;  // declared last: constructed after the state it touches
};

}  // namespace
#endif  // NDEBUG

void install(std::string subsystem) {
    static std::once_flag flag;
    std::call_once(flag, [&] {
        // Resolve subsystem before constructing any sink — autodetect
        // dispatches per platform.
        if (subsystem.empty()) {
#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)
            subsystem = autoDetectSubsystemApple();
#elif defined(__ANDROID__)
            subsystem = autoDetectSubsystemAndroid();
#endif
            if (subsystem.empty()) subsystem = "ge";
        }

        // Build the sink list. stderr stays as a sink everywhere so
        // local-dev console output isn't lost; the native sink fans
        // out to os_log / logcat on mobile.
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)
        sinks.push_back(makeAppleSink(subsystem));
#elif defined(__ANDROID__)
        sinks.push_back(makeAndroidSink(subsystem));
#endif

        // 🎯T83 Dev-only TCP mirror. Opt-in at runtime via LOG_TARGET
        // (host:port); the whole branch is compiled out of release builds.
#ifndef NDEBUG
        const std::string netTarget = resolveLogTarget();
        std::string netHost;
        int netPort = 0;
        // An "appchannel://" target is the structured MessagePack-RPC channel
        // (🎯T92, ge::appchannel) — not this text sink. Skip it here; the
        // appchannel client (installed from ge::run) owns that target.
        if (!netTarget.empty() && netTarget.rfind("appchannel://", 0) != 0) {
            std::tie(netHost, netPort) = parseTarget(netTarget);
            if (netPort > 0)
                sinks.push_back(std::make_shared<NetworkLogSink>(netHost, netPort));
        }
#endif

        // Replace the default logger so SPDLOG_INFO/WARN/ERROR macros
        // (which target spdlog::default_logger_raw()) fan out through
        // every sink in one shot.
        auto logger = std::make_shared<spdlog::logger>(
            "ge", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(std::move(logger));

        // Now that the logger is live, surface the network-sink decision
        // through it (so the line itself also reaches the TCP listener).
#ifndef NDEBUG
        if (netPort > 0) {
            SPDLOG_INFO("ge::log: mirroring to {}:{} via TCP (🎯T83)",
                        netHost, netPort);
        } else if (!netTarget.empty() && netTarget.rfind("appchannel://", 0) != 0) {
            SPDLOG_WARN("ge::log: dev-log target '{}' is not host:port — ignored",
                        netTarget);
        }
#endif
    });
}

} // namespace ge::log
