// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// icon-gen — expand one source SVG into both platforms' app-icon resources
// (🎯T50). Build-time tool; runs as part of `make ge/app-icons`.
//
// Usage:
//   ge-icon-gen --svg <path> [--ios-out <appiconset-dir>]
//                            [--android-res-out <res-dir>]
//                            [--bg-color <#RRGGBB>]
//
// At least one of --ios-out / --android-res-out is required.
//
// iOS output (single-source mode, Xcode 15+):
//   <appiconset-dir>/icon.png         1024×1024 master — OPAQUE RGB, no alpha
//                                     channel (🎯T138: App Store validation
//                                     rejects any alpha on the marketing icon)
//   <appiconset-dir>/Contents.json    universal/ios platform manifest
//
// Android output:
//   mipmap-{m,h,xh,xxh,xxxh}dpi/ic_launcher.png   legacy launcher (48..192 px)
//   mipmap-{...}dpi/ic_launcher_round.png         round legacy variant (same content)
//   drawable/ic_launcher_foreground.png           432×432 adaptive foreground
//                                                  with master SVG centered in
//                                                  the 72dp safe zone (288 px)
//   drawable/ic_launcher_background.xml           solid color (--bg-color) or white
//   mipmap-anydpi-v26/ic_launcher.xml             adaptive manifest
//   mipmap-anydpi-v26/ic_launcher_round.xml       same, round variant
//
// Source PNGs are written non-premultiplied (the convention every PNG reader
// assumes). ge::rasterizeSvgToPixels emits premultiplied RGBA8 — un-premultiply
// before encoding.

#include <ge/svg.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --svg <path> [--ios-out <appiconset-dir>]\n"
        "                       [--android-res-out <res-dir>]\n"
        "                       [--bg-color <#RRGGBB>]\n",
        argv0);
}

bool mkpath(const std::string& path) {
    if (path.empty()) return true;
    std::string buf;
    buf.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        buf.push_back(path[i]);
        if (path[i] == '/' && i > 0) {
            ::mkdir(buf.c_str(), 0755);
        }
    }
    ::mkdir(path.c_str(), 0755);
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        if (!mkpath(path.substr(0, slash))) return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

// Un-premultiply RGBA8 in place. ge::rasterizeSvgToPixels emits premul; PNG
// readers assume non-premul, so flip before encoding.
void unpremultiply(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const uint32_t a = rgba[i + 3];
        if (a == 0) {
            rgba[i + 0] = 0;
            rgba[i + 1] = 0;
            rgba[i + 2] = 0;
        } else if (a < 255) {
            rgba[i + 0] = static_cast<uint8_t>((rgba[i + 0] * 255 + a / 2) / a);
            rgba[i + 1] = static_cast<uint8_t>((rgba[i + 1] * 255 + a / 2) / a);
            rgba[i + 2] = static_cast<uint8_t>((rgba[i + 2] * 255 + a / 2) / a);
        }
    }
}

bool writePng(const std::string& path,
              const std::vector<uint8_t>& rgba,
              int w, int h) {
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        if (!mkpath(path.substr(0, slash))) return false;
    }
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

// 🎯T138 Flatten RGBA → opaque RGB (drops the alpha channel entirely),
// compositing any non-opaque pixel over `bg`. Apple's App Store marketing-icon
// validation rejects a 1024×1024 icon that carries an alpha channel AT ALL —
// even when every pixel is opaque (alpha 255), which a full-bleed rasterized
// icon is. A full-bleed icon has alpha 255 throughout, so `bg` never actually
// shows; it's just a safe fallback for a stray transparent edge.
std::vector<uint8_t> flattenToRgb(const std::vector<uint8_t>& rgba, int w, int h,
                                  uint8_t bg = 255) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<uint8_t> rgb(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; ++c)
            rgb[i * 3 + c] = static_cast<uint8_t>(
                (rgba[i * 4 + c] * a + bg * (255 - a) + 127) / 255);
    }
    return rgb;
}

bool writePngRgb(const std::string& path,
                 const std::vector<uint8_t>& rgb,
                 int w, int h) {
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        if (!mkpath(path.substr(0, slash))) return false;
    }
    return stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3) != 0;
}

// `opaque` (🎯T138): write 3-channel RGB with NO alpha channel — required for
// the iOS App Store marketing icon. Default false keeps the RGBA channel, which
// Android round / adaptive icons need (transparent corners / a masked layer).
bool rasterToPng(const std::string& svg, int size, const std::string& outPath,
                 bool opaque = false) {
    auto pixels = ge::rasterizeSvgToPixels(svg, size, size);
    if (pixels.isNull()) {
        std::fprintf(stderr, "icon-gen: rasterizeSvgToPixels failed at %d×%d for %s\n",
                     size, size, outPath.c_str());
        return false;
    }
    unpremultiply(pixels.rgba);
    const bool ok = opaque
        ? writePngRgb(outPath, flattenToRgb(pixels.rgba, pixels.width, pixels.height),
                      pixels.width, pixels.height)
        : writePng(outPath, pixels.rgba, pixels.width, pixels.height);
    if (!ok) {
        std::fprintf(stderr, "icon-gen: writePng failed for %s\n", outPath.c_str());
        return false;
    }
    return true;
}

// Render the master SVG centered in the 72dp safe zone of a 108dp canvas
// (canvasPx). Visible content occupies (72/108) × canvasPx ≈ 0.667 × canvasPx.
bool rasterToAdaptiveForeground(const std::string& svg,
                                int canvasPx,
                                const std::string& outPath) {
    const int innerPx = (canvasPx * 72 + 54) / 108;  // round to nearest
    auto pixels = ge::rasterizeSvgToPixels(svg, innerPx, innerPx);
    if (pixels.isNull()) {
        std::fprintf(stderr, "icon-gen: rasterizeSvgToPixels failed at %d×%d for adaptive fg\n",
                     innerPx, innerPx);
        return false;
    }
    unpremultiply(pixels.rgba);

    // Compose into a transparent canvasPx × canvasPx buffer with the inner
    // image centered. Padding is fully transparent (rgba = 0,0,0,0).
    std::vector<uint8_t> canvas(static_cast<size_t>(canvasPx) * canvasPx * 4, 0);
    const int offset = (canvasPx - innerPx) / 2;
    for (int y = 0; y < innerPx; ++y) {
        const uint8_t* src = pixels.rgba.data() + static_cast<size_t>(y) * innerPx * 4;
        uint8_t*       dst = canvas.data()
                           + (static_cast<size_t>(y + offset) * canvasPx + offset) * 4;
        std::memcpy(dst, src, static_cast<size_t>(innerPx) * 4);
    }
    return writePng(outPath, canvas, canvasPx, canvasPx);
}

const char* kIosContentsJson = R"JSON({
  "images" : [
    {
      "filename" : "icon.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "ge-icon-gen",
    "version" : 1
  }
}
)JSON";

const char* kAdaptiveIconXml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
)XML";

// Normalize a color string to '#RRGGBB' / '#AARRGGBB'. Empty input → white;
// missing leading '#' is added (Module.mk defaults strip it because Make
// treats '#' as a comment).
std::string normaliseColour(std::string in) {
    if (in.empty()) return "#FFFFFF";
    if (in[0] != '#') in.insert(in.begin(), '#');
    return in;
}

std::string solidColourDrawableXml(const std::string& hex) {
    std::ostringstream oss;
    oss << R"(<?xml version="1.0" encoding="utf-8"?>)" << '\n'
        << R"(<shape xmlns:android="http://schemas.android.com/apk/res/android" android:shape="rectangle">)" << '\n'
        << R"(    <solid android:color=")" << hex << R"("/>)" << '\n'
        << R"(</shape>)" << '\n';
    return oss.str();
}

bool generateIos(const std::string& svg, const std::string& outDir) {
    if (!mkpath(outDir)) {
        std::fprintf(stderr, "icon-gen: cannot create %s\n", outDir.c_str());
        return false;
    }
    // 🎯T138 opaque=true: NO alpha channel — App Store validation rejects any
    // alpha on the 1024 marketing icon. This is the only iOS icon (single-source
    // appiconset), so it's the whole iOS surface; Android keeps its alpha.
    if (!rasterToPng(svg, 1024, outDir + "/icon.png", /*opaque=*/true)) return false;
    if (!writeFile(outDir + "/Contents.json", kIosContentsJson)) {
        std::fprintf(stderr, "icon-gen: failed to write %s/Contents.json\n", outDir.c_str());
        return false;
    }
    return true;
}

bool generateAndroid(const std::string& svg,
                     const std::string& resDir,
                     const std::string& bgHex) {
    struct Density { const char* name; int px; };
    const Density densities[] = {
        {"mdpi",     48},
        {"hdpi",     72},
        {"xhdpi",    96},
        {"xxhdpi",  144},
        {"xxxhdpi", 192},
    };

    // Legacy mipmap PNGs (square, full-bleed).
    for (const auto& d : densities) {
        const std::string mipDir = resDir + "/mipmap-" + d.name;
        if (!rasterToPng(svg, d.px, mipDir + "/ic_launcher.png")) return false;
        if (!rasterToPng(svg, d.px, mipDir + "/ic_launcher_round.png")) return false;
    }

    // Adaptive foreground at xxxhdpi (432px = 108dp × 4). The visible
    // content is the inner 72dp safe zone (288 px); the surrounding
    // 18dp ring is transparent padding so the launcher's mask shape
    // crops the padding, not the icon. Without this the squircle /
    // round mask bites into corner content.
    if (!rasterToAdaptiveForeground(
            svg, 432, resDir + "/drawable/ic_launcher_foreground.png")) {
        return false;
    }

    // Adaptive background — solid color drawable.
    if (!writeFile(resDir + "/drawable/ic_launcher_background.xml",
                   solidColourDrawableXml(bgHex))) {
        std::fprintf(stderr, "icon-gen: failed to write %s/drawable/ic_launcher_background.xml\n",
                     resDir.c_str());
        return false;
    }

    // Adaptive manifests.
    if (!writeFile(resDir + "/mipmap-anydpi-v26/ic_launcher.xml", kAdaptiveIconXml) ||
        !writeFile(resDir + "/mipmap-anydpi-v26/ic_launcher_round.xml", kAdaptiveIconXml)) {
        std::fprintf(stderr, "icon-gen: failed to write %s/mipmap-anydpi-v26/ic_launcher*.xml\n",
                     resDir.c_str());
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string svgPath, iosOut, androidResOut, bgColor = "#FFFFFF";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "icon-gen: %s requires a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if      (arg == "--svg")             svgPath       = next("--svg");
        else if (arg == "--ios-out")         iosOut        = next("--ios-out");
        else if (arg == "--android-res-out") androidResOut = next("--android-res-out");
        else if (arg == "--bg-color")        bgColor       = next("--bg-color");
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "icon-gen: unknown argument %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (svgPath.empty()) {
        std::fprintf(stderr, "icon-gen: --svg is required\n");
        usage(argv[0]);
        return 2;
    }
    if (iosOut.empty() && androidResOut.empty()) {
        std::fprintf(stderr, "icon-gen: at least one of --ios-out / --android-res-out is required\n");
        usage(argv[0]);
        return 2;
    }

    auto lower = svgPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".svg") != 0) {
        std::fprintf(stderr,
                     "icon-gen: source must be .svg (got %s).\n"
                     "          Multi-size resampling of raster sources is not supported;\n"
                     "          author the icon as SVG.\n",
                     svgPath.c_str());
        return 2;
    }

    std::string svg = readFile(svgPath);
    if (svg.empty()) {
        std::fprintf(stderr, "icon-gen: cannot read %s\n", svgPath.c_str());
        return 2;
    }

    if (!iosOut.empty()) {
        std::printf("icon-gen: iOS → %s\n", iosOut.c_str());
        if (!generateIos(svg, iosOut)) return 1;
    }
    if (!androidResOut.empty()) {
        std::printf("icon-gen: Android → %s\n", androidResOut.c_str());
        if (!generateAndroid(svg, androidResOut, normaliseColour(bgColor))) return 1;
    }

    std::printf("icon-gen: done.\n");
    return 0;
}
