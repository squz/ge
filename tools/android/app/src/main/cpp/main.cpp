// Android ge player entry point.
// Scans a QR code on startup to discover the game server, then runs the shared
// player core.
//
// Stream relay address (spyder, default :3030):
//   adb shell am start -n com.squz.player/.GeActivity \
//       --es stream_addr "10.0.2.2:3030" --es server_name "tiltbuggy"
//   (legacy: --es ged_addr "…")
//   adb shell setprop debug.ge.address "192.168.1.100:3030"
//   adb shell setprop debug.ge.server_name "tiltbuggy"
// On the emulator, 10.0.2.2:3030 is used when neither override is present.

#include "player_core.h"
#include "QRScanner.h"
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>
#include <sys/system_properties.h>

#include <jni.h>
#include <SDL3/SDL.h>

namespace {

// Default matches spyder's loopback stream relay (spyder serve).
constexpr int kDefaultPort = 3030;
// Matches ge sample server appName when no override is supplied.
constexpr const char* kDefaultServerName = "tiltbuggy";

bool isEmulator() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("ro.kernel.qemu", value);
    return value[0] == '1';
}

// Parse "host:port" or "host" into a ScanResult.
ge::ScanResult parseAddr(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon != std::string::npos) {
        uint16_t port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
        return {addr.substr(0, colon), port};
    }
    return {addr, static_cast<uint16_t>(kDefaultPort)};
}

// JNI helper: call a static String method on GeActivity; empty on error/null.
std::string intentString(const char* method) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return {};

    jclass cls = env->FindClass("com/squz/player/GeActivity");
    if (!cls) { env->ExceptionClear(); return {}; }

    jmethodID mid = env->GetStaticMethodID(cls, method, "()Ljava/lang/String;");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cls); return {}; }

    jobject obj = env->CallStaticObjectMethod(cls, mid);
    env->DeleteLocalRef(cls);
    if (!obj) return {};

    const char* chars = env->GetStringUTFChars(static_cast<jstring>(obj), nullptr);
    std::string result = chars ? chars : "";
    env->ReleaseStringUTFChars(static_cast<jstring>(obj), chars);
    env->DeleteLocalRef(obj);
    return result;
}

std::string intentStreamAddr() { return intentString("getStreamAddr"); }
std::string intentServerName() { return intentString("getServerName"); }

// Resolve stream server name: intent → debug.ge.server_name → default.
std::string resolveServerName() {
    std::string name = intentServerName();
    if (!name.empty()) return name;
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.ge.server_name", value);
    if (value[0] != '\0') return value;
    return kDefaultServerName;
}

// Check debug.ge.address system property for direct connection (skips QR).
// Set via: adb shell setprop debug.ge.address "192.168.1.100:3030"
// Clear:  adb shell setprop debug.ge.address ""
ge::ScanResult directAddressProp() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.ge.address", value);
    if (value[0] == '\0') return {};
    return parseAddr(std::string(value));
}

int runPlayer(const std::string& host, int port) {
    const std::string name = resolveServerName();
    SPDLOG_INFO("Connecting to {}:{} server_name={}", host, port, name);
    return playerCore(host, port, name);
}

} // namespace

int main(int argc, char* argv[]) {
    auto logger = spdlog::android_logger_mt("ge", "GePlayer");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);

    SPDLOG_INFO("ge player (Android) starting...");

    // Priority 1: stream_addr / ged_addr intent extra.
    {
        std::string addr = intentStreamAddr();
        if (!addr.empty()) {
            auto r = parseAddr(addr);
            SPDLOG_INFO("Intent stream_addr: {}:{}", r.host, r.port);
            return runPlayer(r.host, r.port);
        }
    }

    // Priority 2: System property override — fast, non-blocking.
    {
        auto direct = directAddressProp();
        if (!direct.host.empty()) {
            SPDLOG_INFO("Direct connection via debug.ge.address: {}:{}", direct.host, direct.port);
            return runPlayer(direct.host, direct.port);
        }
    }

    // Priority 3: Emulator auto-connect — Android's alias for the host loopback.
    if (isEmulator()) {
        SPDLOG_INFO("Emulator detected — connecting to 10.0.2.2:{}", kDefaultPort);
        return runPlayer("10.0.2.2", kDefaultPort);
    }

    // Priority 4: Physical device — scan QR code (spyder dashboard / printed URL).
    SPDLOG_INFO("Physical device — waiting for QR scan...");
    ge::ScanResult result = ge::scanQRCode();
    return runPlayer(result.host, result.port);
}
