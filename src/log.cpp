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

// 🎯T119 Dev-only structured log push over spyder's app-channel. The whole
// feature is compiled out of release builds (NDEBUG) so a misconfigured store
// binary can never open a socket to a developer's LAN. See AppChannelLogSink.
#ifndef NDEBUG
#include <ge/appchannel.h>  // structured log push
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <cstdlib>
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

// True iff spyder's app-channel is configured for this process. The address
// comes from the SPYDER_APP_CHANNEL environment variable — spyder's launch_app
// / deploy_app inject it; on Android ge.GeActivity lifts it (and any other
// launch-Intent string-extras) into the process env via setenv before the
// native thread starts, so getenv() works the same on every platform. The
// actual dial + host:port parse live in ge::appchannel; here we only decide
// whether to attach the structured log sink.
bool appChannelConfigured() {
    const char* env = std::getenv("SPYDER_APP_CHANNEL");
    return env && *env;
}

// 🎯T119 Structured-log sink — the only dev-log path. Each spdlog record becomes
// a typed `log` push over spyder's app-channel (T92). The channel's own async
// sender does the socket I/O, so sink_it_ never blocks; ge::appchannel::push
// no-ops until the handshake completes (early lines fall back to the
// always-present native/stderr sink). Attached only when SPYDER_APP_CHANNEL is
// set; compiled out of release builds.
class AppChannelLogSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // Guard against a push path that itself logs (it must not) re-entering.
        static thread_local bool inSink = false;
        if (inSink) return;
        inSink = true;
        const auto lvl = spdlog::level::to_string_view(msg.level);
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
            msg.time.time_since_epoch()).count();
        // spyder's LogPush decodes {ts, level, subsystem?, format} (session.go);
        // `ts` is the on-wire msgpack field name. spdlog pre-renders the message,
        // so the rendered text rides as `format` with no separate args.
        ge::appchannel::push("log", nlohmann::json{
            {"ts",        static_cast<std::int64_t>(ms)},
            {"level",     std::string(lvl.data(), lvl.size())},
            {"subsystem", std::string(msg.logger_name.data(), msg.logger_name.size())},
            {"format",    std::string(msg.payload.data(), msg.payload.size())},
        });
        inSink = false;
    }
    void flush_() override {}
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

        // 🎯T119 Dev-only structured log push over spyder's app-channel.
        // Opt-in at runtime via SPYDER_APP_CHANNEL; the channel itself is dialed
        // by ge::appchannel from ge::run. Compiled out of release builds.
#ifndef NDEBUG
        const bool appChannel = appChannelConfigured();
        if (appChannel)
            sinks.push_back(std::make_shared<AppChannelLogSink>());
#endif

        // Replace the default logger so SPDLOG_INFO/WARN/ERROR macros
        // (which target spdlog::default_logger_raw()) fan out through
        // every sink in one shot.
        auto logger = std::make_shared<spdlog::logger>(
            "ge", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(std::move(logger));

        // Now that the logger is live, surface the app-channel decision
        // through it (so the line itself also reaches spyder).
#ifndef NDEBUG
        if (appChannel)
            SPDLOG_INFO("ge::log: structured log push via spyder app-channel");
#endif
    });
}

} // namespace ge::log
