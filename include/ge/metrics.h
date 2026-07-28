// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T166 — per-instance assignable metric producers + zero-I/O ring capture.
//
// DX:
//   ge::metrics::Scope scope;                 // one per game instance
//   ge::metrics::metric<float> dt{scope, "dt"};
//   dt = frameDt;                             // no-op when series unarmed
//   scope.endFrame();                         // once per frame after assigns
//
// Arm/list/dump via app-channel methods (metrics_*) when SPYDER_APP_CHANNEL is
// set. Entire surface is a no-op under NDEBUG.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ge::metrics {

enum class Kind : std::uint8_t { Float = 0, U16 = 1, Bool = 2 };

// Per game-instance capture scope (ring + catalog). Not process-global data.
class Scope {
public:
    // Optional stable id for multi-instance targeting (defaults to unique hex).
    explicit Scope(std::string id = {});
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) noexcept;
    Scope& operator=(Scope&&) noexcept;

    const std::string& id() const;

    // Catalog of registered series (name + kind string).
    nlohmann::json list() const;

    // Arm selected series (by name) with a ring capacity in frames.
    // Unknown names are ignored. Capacity clamped to [1, 1e6].
    void arm(const std::vector<std::string>& series, std::size_t capacity);

    void disarm();

    // { instance, armed, capacity, count, series: [...] }
    nlohmann::json status() const;

    // Full retained history for armed series only:
    // { instance, series, kinds, frames: [[...], ...], count, capacity }
    nlohmann::json dump() const;

    // Advance the ring by one frame if any armed series was written this frame.
    void endFrame();

    bool anyArmed() const;

    // Registry of live scopes (for app-channel routing).
    static std::vector<Scope*> all();
    // T175.9 The scopes visible to one session: those constructed while it
    // was the sole live session, plus untagged (process-wide) scopes.
    static std::vector<Scope*> all(uint32_t sessionId);
    static Scope* find(std::string_view id);
    // T175.9 Session tag, latched at construction (0 = process-wide).
    uint32_t session() const;

    // Used by metric<T>; also tests.
    int registerSeries(std::string_view name, Kind kind);
    void write(int seriesIndex, double value);
    void setActive(int seriesIndex, bool active);
    bool isActive(int seriesIndex) const;

private:
    struct M;
    M* m_ = nullptr; // raw for move; owned
};

// Typed producer bound to a Scope. Construction registers `name`.
template <typename T>
class metric {
public:
    metric(Scope& scope, const char* name)
        : scope_(&scope)
        , index_(scope.registerSeries(name, kindOf())) {}

    metric(const metric&) = delete;
    metric& operator=(const metric&) = delete;
    metric(metric&&) = default;
    metric& operator=(metric&&) = default;

    metric& operator=(T value) {
        if (scope_ && scope_->isActive(index_))
            scope_->write(index_, toDouble(value));
        return *this;
    }

private:
    static Kind kindOf() {
        if constexpr (std::is_same_v<T, bool>)
            return Kind::Bool;
        else if constexpr (std::is_same_v<T, std::uint16_t> || std::is_same_v<T, int>)
            return Kind::U16;
        else
            return Kind::Float;
    }
    static double toDouble(T v) {
        if constexpr (std::is_same_v<T, bool>)
            return v ? 1.0 : 0.0;
        else
            return static_cast<double>(v);
    }

    Scope* scope_ = nullptr;
    int index_ = -1;
};

} // namespace ge::metrics
