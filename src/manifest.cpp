// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// manifest.cpp — 🎯T64.5 build manifest reader implementation

#include <ge/manifest.h>
#include <ge/Resource.h>
#include <ge/FileIO.h>

#include <mutex>
#include <sstream>
#include <iterator>

namespace ge {

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON field extractor.
//
// The manifest is tiny and machine-generated (well-formed by construction),
// so a full JSON parser is overkill. We extract each key with a simple
// string-search approach: find `"key"` followed by `:` followed by `"value"`.
// This is adequate for the flat BuildManifest schema; if the schema ever
// grows nested objects, replace with nlohmann/json or similar.
// ---------------------------------------------------------------------------
static std::string extractStringField(const std::string& json, const std::string& key) {
    // Search for `"key"` in the JSON.
    std::string needle = "\"" + key + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return {};
    }
    // Find the colon after the key.
    auto colon_pos = json.find(':', key_pos + needle.size());
    if (colon_pos == std::string::npos) {
        return {};
    }
    // Find the opening quote of the value.
    auto open_quote = json.find('"', colon_pos + 1);
    if (open_quote == std::string::npos) {
        return {};
    }
    // Find the closing quote (not preceded by a backslash).
    auto close_quote = open_quote + 1;
    while (close_quote < json.size()) {
        if (json[close_quote] == '"' && json[close_quote - 1] != '\\') {
            break;
        }
        ++close_quote;
    }
    if (close_quote >= json.size()) {
        return {};
    }
    return json.substr(open_quote + 1, close_quote - open_quote - 1);
}

// ---------------------------------------------------------------------------
// Try to read manifest.json from one of the platform-appropriate paths.
// Returns empty string if not found.
// ---------------------------------------------------------------------------
static std::string readManifestJson() {
    // Try the xcassets dataset path first (matches what ship/manifest.sh writes
    // when embedding into iOS Assets.xcassets/manifest.dataset/manifest.json).
    static const char* kPaths[] = {
        "manifest.dataset/manifest.json",
        "manifest.json",
        nullptr,
    };

    for (const char** p = kPaths; *p != nullptr; ++p) {
        auto stream = ge::openFile(ge::resource(*p));
        if (!stream) {
            continue;
        }
        std::ostringstream oss;
        oss << stream->rdbuf();
        std::string content = oss.str();
        if (!content.empty()) {
            return content;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Parse a BuildManifest from a JSON string. Returns nullopt on parse failure.
// ---------------------------------------------------------------------------
static std::optional<BuildManifest> parseManifest(const std::string& json) {
    BuildManifest m;
    m.tag                 = extractStringField(json, "tag");
    m.git_sha             = extractStringField(json, "git_sha");
    m.git_branch          = extractStringField(json, "git_branch");
    m.build_timestamp_utc = extractStringField(json, "build_timestamp_utc");
    m.ge_version          = extractStringField(json, "ge_version");
    m.lane                = extractStringField(json, "lane");
    m.builder             = extractStringField(json, "builder");

    // Require at minimum the tag and git_sha to consider the parse successful.
    if (m.tag.empty() || m.git_sha.empty()) {
        return std::nullopt;
    }
    return m;
}

} // namespace

std::optional<BuildManifest> readBuildManifest() {
    static std::once_flag s_flag;
    static std::optional<BuildManifest> s_result;

    std::call_once(s_flag, []() {
        auto json = readManifestJson();
        if (!json.empty()) {
            s_result = parseManifest(json);
        }
    });

    return s_result;
}

} // namespace ge
