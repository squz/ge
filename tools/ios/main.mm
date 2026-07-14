// iOS ge player entry point.
// Scans a QR code on startup to discover the game server, then runs the shared player core.
//
// Stream relay address (spyder, default :3030):
//   -stream_addr host:port   (preferred; simctl/devicectl launch arg)
//   -ged_addr host:port      (legacy alias)
//   GE_STREAM_ADDR=host:port (preferred env)
//   GE_DAEMON_ADDR=host:port (legacy env)
// Example (simulator):  xcrun simctl launch <udid> com.squz.player -stream_addr 192.168.1.217:3030
// Example (device):     xcrun devicectl device process launch --console-pty -- com.squz.player -stream_addr 192.168.1.217:3030
// Example (env var):    xcrun devicectl device process launch -e '{"GE_STREAM_ADDR":"192.168.1.217:3030"}' ...

#include <TargetConditionals.h>
#include "player_core.h"
#include "QRScanner.h"
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#import <Foundation/Foundation.h>

#include <cstdlib>
#include <string>
#include <thread>

// HTTP PUT sink — sends each log line to a logging server on the host.
// Uses the stream-relay host (or localhost for simulator).
static std::string g_logHost = "192.168.1.217";

template<typename Mutex>
class http_sink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        std::string body = fmt::to_string(formatted);

        std::string urlCpp = "http://" + g_logHost + ":9999/log";
        std::thread([body = std::move(body), urlCpp = std::move(urlCpp)] {
            @autoreleasepool {
                NSString* urlStr = [NSString stringWithUTF8String:urlCpp.c_str()];
                NSURL* url = [NSURL URLWithString:urlStr];
                NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
                req.HTTPMethod = @"PUT";
                req.HTTPBody = [NSData dataWithBytes:body.c_str() length:body.size()];
                req.timeoutInterval = 1.0;

                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [[NSURLSession.sharedSession dataTaskWithRequest:req
                    completionHandler:^(NSData*, NSURLResponse*, NSError*) {
                        dispatch_semaphore_signal(sem);
                    }] resume];
                dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
            }
        }).detach();
    }
    void flush_() override {}
};

// Default matches spyder's loopback stream relay (spyder serve).
static constexpr uint16_t kDefaultPort = 3030;

int main(int argc, char* argv[]) {
    auto sink = std::make_shared<http_sink<std::mutex>>();
    auto logger = std::make_shared<spdlog::logger>("player", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);

    SPDLOG_INFO("ge player (iOS) starting...");

    std::string host;
    uint16_t port = kDefaultPort;

    // Helper: parse "host:port" or "host" into host/port.
    auto parseAddr = [&](const std::string& s) {
        host = s;
        if (auto colon = s.rfind(':'); colon != std::string::npos) {
            host = s.substr(0, colon);
            port = static_cast<uint16_t>(std::stoi(s.substr(colon + 1)));
        }
    };

    // Priority 1: launch arg -stream_addr (preferred) or legacy -ged_addr.
    // simctl stores launch args as NSUserDefaults. Read once and do not persist.
    @autoreleasepool {
        NSUserDefaults* defs = [NSUserDefaults standardUserDefaults];
        NSString* key = @"stream_addr";
        NSString* addr = [defs stringForKey:key];
        if (!addr || addr.length == 0) {
            key = @"ged_addr";
            addr = [defs stringForKey:key];
        }
        if (addr && addr.length > 0) {
            parseAddr(std::string(addr.UTF8String));
            SPDLOG_INFO("{} launch arg: {}:{}", key.UTF8String, host, port);
            [defs removeObjectForKey:key];
        }
    }

    // Priority 2: GE_STREAM_ADDR (preferred) or legacy GE_DAEMON_ADDR.
    if (host.empty()) {
        if (const char* addr = std::getenv("GE_STREAM_ADDR")) {
            parseAddr(std::string(addr));
            SPDLOG_INFO("GE_STREAM_ADDR: {}:{}", host, port);
        } else if (const char* addr = std::getenv("GE_DAEMON_ADDR")) {
            parseAddr(std::string(addr));
            SPDLOG_INFO("GE_DAEMON_ADDR (legacy): {}:{}", host, port);
        }
    }

    // Priority 3: Platform-specific fallback.
    if (host.empty()) {
#if TARGET_OS_SIMULATOR
        SPDLOG_INFO("Simulator: using localhost:{}", kDefaultPort);
        host = "localhost";
        port = kDefaultPort;
#else
        // Blocking QR scan to discover the server.
        ge::ScanResult result = ge::scanQRCode();
        host = result.host;
        port = result.port;
#endif
    }

    // Catalogue name (must match server registration, e.g. tiltbuggy).
    // Override with GE_SERVER_NAME; default matches Android player.
    std::string serverName = "tiltbuggy";
    if (const char* n = std::getenv("GE_SERVER_NAME")) {
        if (n[0]) serverName = n;
    }
    SPDLOG_INFO("server name: {}", serverName);

    return playerCore(host, port, serverName);
}
