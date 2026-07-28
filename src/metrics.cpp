// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T166 per-instance metrics ring implementation.

#include <ge/metrics.h>

#include <ge/SessionHost.h>  // T175.9 liveSessionIds

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ge::metrics {
namespace {

std::mutex g_regMu;
std::vector<Scope*> g_scopes;
std::uint64_t g_nextId = 1;

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Float: return "float";
        case Kind::U16:   return "u16";
        case Kind::Bool:  return "bool";
    }
    return "float";
}

} // namespace

struct Scope::M {
    std::string id;
    uint32_t session = 0;  // T175.9 latched at construction
    struct Series {
        std::string name;
        Kind kind = Kind::Float;
        bool active = false;
        int col = -1; // column in ring when armed
    };
    std::vector<Series> series;

    bool captureArmed = false;
    std::size_t capacity = 0;
    std::size_t count = 0;   // frames stored (≤ capacity)
    std::size_t head = 0;    // next write index
    std::vector<std::string> armedNames;
    std::vector<Kind> armedKinds;
    // Flat ring: capacity * nCols doubles
    std::vector<double> ring;
    std::vector<double> staging; // current frame
    bool dirty = false;
    int nCols = 0;

    void clearRing() {
        ring.clear();
        staging.clear();
        capacity = 0;
        count = 0;
        head = 0;
        nCols = 0;
        dirty = false;
        captureArmed = false;
        armedNames.clear();
        armedKinds.clear();
        for (auto& s : series) {
            s.active = false;
            s.col = -1;
        }
    }
};

namespace {
uint32_t lenientMetricsSid() {
    const auto live = liveSessionIds();
    return live.size() == 1 ? live.front() : 0u;
}
}  // namespace

Scope::Scope(std::string id) {
    m_ = new M;
    if (id.empty()) {
        std::ostringstream os;
        os << "inst-" << g_nextId++;
        m_->id = os.str();
    } else {
        m_->id = std::move(id);
    }
    m_->session = lenientMetricsSid();  // T175.9
    std::lock_guard<std::mutex> lk(g_regMu);
    g_scopes.push_back(this);
}

Scope::~Scope() {
    if (!m_) return;
    {
        std::lock_guard<std::mutex> lk(g_regMu);
        g_scopes.erase(std::remove(g_scopes.begin(), g_scopes.end(), this), g_scopes.end());
    }
    delete m_;
    m_ = nullptr;
}

Scope::Scope(Scope&& o) noexcept : m_(o.m_) {
    o.m_ = nullptr;
    if (m_) {
        std::lock_guard<std::mutex> lk(g_regMu);
        for (auto& p : g_scopes)
            if (p == &o) p = this;
    }
}

Scope& Scope::operator=(Scope&& o) noexcept {
    if (this == &o) return *this;
    if (m_) {
        std::lock_guard<std::mutex> lk(g_regMu);
        g_scopes.erase(std::remove(g_scopes.begin(), g_scopes.end(), this), g_scopes.end());
        delete m_;
    }
    m_ = o.m_;
    o.m_ = nullptr;
    if (m_) {
        std::lock_guard<std::mutex> lk(g_regMu);
        for (auto& p : g_scopes)
            if (p == &o) p = this;
        // ensure this is registered
        if (std::find(g_scopes.begin(), g_scopes.end(), this) == g_scopes.end())
            g_scopes.push_back(this);
    }
    return *this;
}

const std::string& Scope::id() const { return m_->id; }

std::vector<Scope*> Scope::all() {
    std::lock_guard<std::mutex> lk(g_regMu);
    return g_scopes;
}

std::vector<Scope*> Scope::all(uint32_t sessionId) {
    std::lock_guard<std::mutex> lk(g_regMu);
    std::vector<Scope*> out;
    for (Scope* s : g_scopes)
        if (s->m_ && (s->m_->session == 0 || s->m_->session == sessionId))
            out.push_back(s);
    return out;
}

uint32_t Scope::session() const { return m_->session; }

Scope* Scope::find(std::string_view id) {
    std::lock_guard<std::mutex> lk(g_regMu);
    for (Scope* s : g_scopes)
        if (s->m_ && s->m_->id == id) return s;
    return nullptr;
}

int Scope::registerSeries(std::string_view name, Kind kind) {
    if (!m_ || name.empty()) return -1;
    for (int i = 0; i < (int)m_->series.size(); ++i) {
        if (m_->series[i].name == name) {
            m_->series[i].kind = kind;
            return i;
        }
    }
    m_->series.push_back(M::Series{std::string(name), kind, false, -1});
    return (int)m_->series.size() - 1;
}

bool Scope::isActive(int seriesIndex) const {
    if (!m_ || seriesIndex < 0 || seriesIndex >= (int)m_->series.size())
        return false;
    return m_->series[seriesIndex].active;
}

void Scope::setActive(int seriesIndex, bool active) {
    if (!m_ || seriesIndex < 0 || seriesIndex >= (int)m_->series.size())
        return;
    m_->series[seriesIndex].active = active;
}

void Scope::write(int seriesIndex, double value) {
    if (!m_ || !m_->captureArmed) return;
    if (seriesIndex < 0 || seriesIndex >= (int)m_->series.size()) return;
    const auto& s = m_->series[seriesIndex];
    if (!s.active || s.col < 0) return;
    if (!std::isfinite(value)) value = 0.0;
    m_->staging[(std::size_t)s.col] = value;
    m_->dirty = true;
}

void Scope::endFrame() {
    if (!m_ || !m_->captureArmed || !m_->dirty || m_->nCols <= 0 || m_->capacity == 0)
        return;
    const std::size_t base = m_->head * (std::size_t)m_->nCols;
    for (int c = 0; c < m_->nCols; ++c)
        m_->ring[base + (std::size_t)c] = m_->staging[(std::size_t)c];
    m_->head = (m_->head + 1) % m_->capacity;
    if (m_->count < m_->capacity) ++m_->count;
    // reset staging to 0 for next frame (optional)
    std::fill(m_->staging.begin(), m_->staging.end(), 0.0);
    m_->dirty = false;
}

bool Scope::anyArmed() const {
    return m_ && m_->captureArmed;
}

nlohmann::json Scope::list() const {
    nlohmann::json arr = nlohmann::json::array();
    if (!m_) return arr;
    for (const auto& s : m_->series) {
        arr.push_back({{"name", s.name}, {"kind", kindName(s.kind)}});
    }
    return nlohmann::json{
        {"instance", m_->id},
        {"series", std::move(arr)},
    };
}

void Scope::arm(const std::vector<std::string>& series, std::size_t capacity) {
    if (!m_) return;
    m_->clearRing();
    if (capacity < 1) capacity = 1;
    if (capacity > 1000000) capacity = 1000000;

    std::vector<int> indices;
    for (const auto& name : series) {
        for (int i = 0; i < (int)m_->series.size(); ++i) {
            if (m_->series[i].name == name) {
                indices.push_back(i);
                break;
            }
        }
    }
    if (indices.empty()) return;

    m_->nCols = (int)indices.size();
    m_->capacity = capacity;
    m_->ring.assign(capacity * (std::size_t)m_->nCols, 0.0);
    m_->staging.assign((std::size_t)m_->nCols, 0.0);
    m_->count = 0;
    m_->head = 0;
    m_->dirty = false;
    m_->captureArmed = true;

    for (int col = 0; col < m_->nCols; ++col) {
        int si = indices[(std::size_t)col];
        m_->series[si].active = true;
        m_->series[si].col = col;
        m_->armedNames.push_back(m_->series[si].name);
        m_->armedKinds.push_back(m_->series[si].kind);
    }
}

void Scope::disarm() {
    if (!m_) return;
    m_->clearRing();
}

nlohmann::json Scope::status() const {
    if (!m_) return nlohmann::json::object();
    nlohmann::json series = nlohmann::json::array();
    for (const auto& n : m_->armedNames) series.push_back(n);
    return nlohmann::json{
        {"instance", m_->id},
        {"armed", m_->captureArmed},
        {"capacity", m_->capacity},
        {"count", m_->count},
        {"series", std::move(series)},
    };
}

nlohmann::json Scope::dump() const {
    if (!m_) return nlohmann::json::object();
    nlohmann::json series = nlohmann::json::array();
    nlohmann::json kinds = nlohmann::json::array();
    for (std::size_t i = 0; i < m_->armedNames.size(); ++i) {
        series.push_back(m_->armedNames[i]);
        kinds.push_back(kindName(m_->armedKinds[i]));
    }
    nlohmann::json frames = nlohmann::json::array();
    if (m_->count == 0 || m_->nCols == 0) {
        return nlohmann::json{
            {"instance", m_->id},
            {"series", std::move(series)},
            {"kinds", std::move(kinds)},
            {"frames", std::move(frames)},
            {"count", 0},
            {"capacity", m_->capacity},
        };
    }
    // Oldest first: if not full, [0 .. count); if full, [head .. head+cap)
    const std::size_t start = (m_->count < m_->capacity) ? 0 : m_->head;
    for (std::size_t i = 0; i < m_->count; ++i) {
        const std::size_t fi = (start + i) % m_->capacity;
        nlohmann::json row = nlohmann::json::array();
        const std::size_t base = fi * (std::size_t)m_->nCols;
        for (int c = 0; c < m_->nCols; ++c)
            row.push_back(m_->ring[base + (std::size_t)c]);
        frames.push_back(std::move(row));
    }
    return nlohmann::json{
        {"instance", m_->id},
        {"series", std::move(series)},
        {"kinds", std::move(kinds)},
        {"frames", std::move(frames)},
        {"count", m_->count},
        {"capacity", m_->capacity},
    };
}

} // namespace ge::metrics
