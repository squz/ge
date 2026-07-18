// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T157: web (Emscripten/wasm) implementation of ge::resolveFont.
//
// The browser exposes no system font files — the wasm virtual FS contains
// only what the app preloaded. So `system:` logical names resolve by
// convention to app-shipped files under fonts/ (resolved via ge::resource,
// i.e. preloaded with --preload-file fonts@/fonts):
//
//   system:sans-serif       → fonts/sans-serif.ttf
//   system:sans-serif-bold  → fonts/sans-serif-bold.ttf   (falls back to regular)
//   … same pattern for serif / monospace.
//
// Apps that want real typography ship their own faces anyway
// (ge::registerSvgFontFace); this mapping just keeps SVG <text> and
// rasterizeText working out of the box when the conventional files exist.

#if defined(__EMSCRIPTEN__)

#include <ge/FontLoader.h>
#include <ge/Resource.h>

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ge {
namespace {

std::vector<std::string> candidatesFor(const std::string& name) {
    static const char* kLogical[] = {
        "sans-serif", "sans-serif-bold",
        "serif",      "serif-bold",
        "monospace",  "monospace-bold",
    };
    for (const char* logical : kLogical) {
        if (name == logical) {
            std::vector<std::string> out;
            out.push_back(ge::resource("fonts/" + name + ".ttf"));
            // A bold logical name falls back to the regular cut (faux-bold
            // downstream) so shipping one file per family is enough.
            if (name.ends_with("-bold")) {
                out.push_back(ge::resource(
                    "fonts/" + name.substr(0, name.size() - strlen("-bold")) + ".ttf"));
            }
            return out;
        }
    }
    return {};
}

FontRef resolveSystemFont(const std::string& name) {
    const auto candidates = candidatesFor(name);
    if (candidates.empty()) {
        throw std::runtime_error(
            "ge::resolveFont: unknown system font name '" + name + "'");
    }
    for (const auto& path : candidates) {
        if (access(path.c_str(), R_OK) == 0) {
            return FontRef{path, 0};
        }
    }
    throw std::runtime_error(
        "ge::resolveFont: no preloaded candidate for system font '" + name +
        "' — the web build has no OS fonts; ship fonts/" + name +
        ".ttf in the preloaded FS or register app faces via registerSvgFontFace");
}

} // namespace

FontRef resolveFont(const std::string& uri) {
    // Cache positive and negative results (same contract as the other
    // platforms): the answer never changes within a process, and an empty
    // FontRef marks a URI that already failed.
    static std::unordered_map<std::string, FontRef> cache;
    static std::mutex cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = cache.find(uri); it != cache.end()) {
            if (it->second.path.empty()) {
                throw std::runtime_error(
                    "ge::resolveFont: '" + uri + "' previously failed to resolve");
            }
            return it->second;
        }
    }

    constexpr const char* kSystemPrefix = "system:";
    constexpr const char* kFilePrefix = "file:";

    FontRef result;
    try {
        if (uri.starts_with(kSystemPrefix)) {
            result = resolveSystemFont(uri.substr(strlen(kSystemPrefix)));
        } else if (uri.starts_with(kFilePrefix)) {
            result = FontRef{uri.substr(strlen(kFilePrefix)), 0};
        } else {
            result = FontRef{ge::resource(uri), 0};
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache.emplace(uri, FontRef{});
        throw;
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.emplace(uri, result);
    return result;
}

} // namespace ge

#endif // __EMSCRIPTEN__
