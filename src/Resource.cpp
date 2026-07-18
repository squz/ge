#include <ge/Resource.h>
#include <SDL3/SDL.h>
#include "sokol_gfx.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ge {

std::string resource(const std::string& relativePath) {
    // Already absolute — return unchanged.
    if (!relativePath.empty() && relativePath[0] == '/') {
        return relativePath;
    }

    static const std::string base = [] {
#if defined(__ANDROID__)
        // On Android, assets live inside the APK. SDL_IOFromFile handles
        // them via AssetManager when given a relative path — no prefix needed.
        return std::string();
#endif
#if defined(__EMSCRIPTEN__)
        // 🎯T157 Web: assets are preloaded into the wasm virtual FS at
        // absolute paths mirroring the project layout (--preload-file
        // assets@/assets etc.), so the project root is the FS root — not
        // the "up one level from the binary" desktop heuristic below.
        return std::string("/");
#endif
        auto p = SDL_GetBasePath();
        if (!p) return std::string();
        std::string dir(p);
#if (defined(__APPLE__) && TARGET_OS_IOS)
        // SDL_GetBasePath() returns the app bundle Resources/ directory.
        return dir;
#else
        // SDL_GetBasePath() returns the binary's directory, e.g. "/path/to/bin/".
        // Go up one level to the project root (convention: binary lives in bin/).
        if (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        auto pos = dir.rfind('/');
        return pos != std::string::npos ? dir.substr(0, pos + 1) : std::string();
#endif
    }();

    return base + relativePath;
}

namespace {
const char* shaderProfileSuffix() {
#if defined(__ANDROID__)
    switch (sg_query_backend()) {
    case SG_BACKEND_VULKAN: return "-spirv";
    case SG_BACKEND_GLES3:  return "-gles";
    default: break;  // shouldn't happen on Android
    }
    return "-gles";   // safer fallback (no SPIR-V mismatch)
#elif defined(__EMSCRIPTEN__)
    return "-gles";   // 🎯T157 WebGL2 == the GLES3 profile, single backend
#else
    return "";        // Apple → Metal, single canonical "shaders/" dir
#endif
}
}

std::string shaderDir() {
    return std::string("build/shaders") + shaderProfileSuffix();
}

std::string renderShaderDir() {
    return std::string("build/ge/shaders") + shaderProfileSuffix();
}

} // namespace ge
