// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::iap floor: backend dispatcher + StubStore implementation +
// DebugPanel. Platform backends in iap_apple.mm and iap_android.cpp
// implement Store and provide makePlatformStore(); the dispatcher
// picks one at process startup based on GE_IAP_MODE.

#include "iap_internal.h"

#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cassert>
#include <cstdlib>

namespace ge::iap {

namespace detail {

// In-memory backend. CI default, debug-menu backing store, unit-test
// substrate. Threadsafe under a coarse mutex — IAP is not a hot path.
struct StubStore : Store {
    mutable std::mutex                              mu;
    std::unordered_map<std::string, Product>        catalogue;
    std::unordered_set<std::string>                 entitled;

    void setCatalogue(std::vector<Product> next) override {
        std::lock_guard lock(mu);
        for (const auto& p : next) {
            auto [it, inserted] = catalogue.try_emplace(p.id, p);
            assert(inserted || it->second.type == p.type);
        }
    }

    bool owned(const std::string& id) const override {
        std::lock_guard lock(mu);
        return entitled.contains(id);
    }

    std::vector<LocalisedProduct> products() const override {
        std::lock_guard lock(mu);
        std::vector<LocalisedProduct> out;
        out.reserve(catalogue.size());
        // Synthetic prices so dev UI can render. Real prices come
        // from the platform store when GE_IAP_MODE=platform.
        for (const auto& [id, p] : catalogue) {
            out.push_back({
                .id          = id,
                .title       = id,
                .description = "(stub) " + id,
                .price       = "$0.99",
                .currency    = "USD",
            });
        }
        return out;
    }

    void buy(const std::string& id, BuyCallback cb) override {
        Result r;
        {
            std::lock_guard lock(mu);
            if (catalogue.find(id) == catalogue.end()) {
                r = {.ok = false, .id = id, .error = "unknown product"};
            } else {
                const Product& p = catalogue.at(id);
                if (p.type != Type::Consumable) {
                    entitled.insert(id);
                }
                r = {.ok = true, .id = id, .error = {}};
            }
        }
        if (cb) cb(std::move(r));
    }

    void restore(RestoreCallback cb) override {
        if (cb) cb({.ok = true, .id = {}, .error = {}});
    }

    void testingSetOwned(const std::string& id, bool yes) override {
        std::lock_guard lock(mu);
        if (yes) entitled.insert(id);
        else     entitled.erase(id);
    }

    void testingClearAll() override {
        std::lock_guard lock(mu);
        entitled.clear();
    }
};

#if !defined(__APPLE__) && !defined(__ANDROID__)
// Desktop / fallback default: no platform store. iap_apple.mm and
// iap_android.cpp override this on their platforms.
std::unique_ptr<Store> makePlatformStore() { return nullptr; }
#endif

#if !defined(__APPLE__)
// 🎯T74: only iOS has a meaningful storekit-bundled check.
bool storekitConfigBundled() { return false; }
#endif

} // namespace detail

namespace {

using detail::Store;
using detail::StubStore;

// Single backend instance, lazily constructed on first access.
// std::once_flag guarantees the env-var probe + selection happens
// exactly once even under concurrent first-touch from multiple threads.
Store& store() {
    static std::unique_ptr<Store> instance;
    static std::once_flag         init;
    std::call_once(init, [] {
        const char* envMode = std::getenv("GE_IAP_MODE");
        std::string mode    = envMode ? envMode : "";
        // Default mode: platform on mobile, stub on desktop. macOS is
        // desktop here — only iOS / iPadOS / tvOS / watchOS qualify
        // as "mobile" for the auto-platform default.
        if (mode.empty()) {
#if defined(__ANDROID__)
            mode = "platform";
#elif defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)
            // 🎯T74: prefer local mode when a StoreKit.storekit is bundled
            // with the iOS app — lets devs test the real StoreKit code path
            // on devices without a cached sandbox Apple account. Production
            // builds (which can't ship .storekit through App Store Connect)
            // fall through to "platform" automatically.
            // 🎯T74: treat the presence of a bundled StoreKit.storekit as
            // "this is a dev/test build" and route to stub. The intended
            // long-term behaviour is to route those to "local" mode
            // (SKTestSession-backed real StoreKit calls) — but SKTestSession
            // doesn't reliably intercept from a non-XCTest app process today,
            // so the inner production bridge still triggers a sandbox sign-in
            // modal at launch on devices without a cached sandbox Apple
            // account. Stub mode skips StoreKit entirely → no modal, no IAP
            // (BUY-PRO button visible, tapping it no-ops). Production builds
            // (which can't ship .storekit through App Store Connect) fall
            // through to "platform" automatically.
            const bool sk = detail::storekitConfigBundled();
            mode = sk ? "stub" : "platform";
            SPDLOG_INFO("ge::iap: T74 auto-mode iOS storekitConfigBundled={} mode={}", sk, mode);
#else
            mode = "stub";
#endif
            // Propagate the auto-picked mode to platform impls that re-read
            // the env var on construction (iap_apple.mm checks getenv to
            // choose between GEStoreKit2BridgeImpl and the Local variant).
            // overwrite=1 because the env var is unset when launched outside
            // Xcode (e.g., via spyder deploy); only Xcode-scheme launches set
            // it explicitly, and even then they win because they preempt this
            // default-only branch via the envMode check above.
            ::setenv("GE_IAP_MODE", mode.c_str(), 1);
        }
        SPDLOG_INFO("ge::iap: mode resolved to '{}'", mode);

        if (mode == "stub") {
            instance = std::make_unique<StubStore>();
        } else if (mode == "platform") {
            instance = detail::makePlatformStore();
            if (!instance) {
                SPDLOG_WARN("GE_IAP_MODE=platform but no platform store available; falling back to stub");
                instance = std::make_unique<StubStore>();
            }
        } else if (mode == "local") {
            // local mode (🎯T65.4): iOS reads from StoreKit.storekit via
            // SKTestSession (GEStoreKit2LocalBridgeImpl); Android routes
            // buy() through android.test.* reserved SKUs. On desktop there
            // is no platform store, so local falls through to stub.
            instance = detail::makePlatformStore();
            if (!instance) {
                SPDLOG_WARN("GE_IAP_MODE=local but no platform store available on this platform; falling back to stub");
                instance = std::make_unique<StubStore>();
            }
        } else {
            SPDLOG_WARN("GE_IAP_MODE={} not recognised; falling back to stub", mode);
            instance = std::make_unique<StubStore>();
        }
    });
    return *instance;
}

} // namespace

void setCatalogue(std::vector<Product> c)                     { store().setCatalogue(std::move(c)); }
bool owned(const std::string& id)                             { return store().owned(id); }
std::vector<LocalisedProduct> products()                      { return store().products(); }
void buy(const std::string& id, BuyCallback cb)               { store().buy(id, std::move(cb)); }
void restore(RestoreCallback cb)                              { store().restore(std::move(cb)); }

namespace testing {

void setOwned(const std::string& id, bool yes) { store().testingSetOwned(id, yes); }
void clearAll()                                { store().testingClearAll(); }

} // namespace testing

// ── DebugPanel ─────────────────────────────────────────────────────

std::vector<DebugPanel::Row> DebugPanel::rows() const {
    auto list = products();
    std::vector<Row> out;
    out.reserve(list.size());
    for (const auto& p : list) {
        out.push_back({.id = p.id, .type = Type::NonConsumable, .owned = owned(p.id)});
    }
    return out;
}

void DebugPanel::onRowTap(const std::string& id)        { testing::setOwned(id, !owned(id)); }
void DebugPanel::onResetAll()                            { testing::clearAll(); }
void DebugPanel::onForceRestore(RestoreCallback cb)      { restore(std::move(cb)); }

} // namespace ge::iap
