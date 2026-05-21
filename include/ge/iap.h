// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Cross-platform in-app purchases. The consumer game registers a
// product catalogue, queries owned() in O(1) against a cached
// entitlement set, and initiates purchases through buy() with a
// callback. The same code compiles and runs on macOS (StubStore),
// iOS (StoreKit 2), and Android (Play Billing) without #ifdef.
//
// Backend selection happens once at init from the GE_IAP_MODE env var:
//   stub      — pure C++ in-memory, no platform calls. CI default.
//   local     — .storekit (iOS) / android.test.* (Android). Dev iteration.
//   platform  — real StoreKit / Play Billing. Release default on mobile.
// On platforms where the requested backend isn't implemented, the
// dispatcher falls back to stub and logs a warning.
//
// Product IDs are local — the consumer writes "pro", and ge prepends
// the bundle ID to form the platform SKU "com.squz.tiltbuggy.pro".
// The same string registered in App Store Connect and Play Console
// is what appears in the game's source — no separate mapping table.
//
// Threat model: the platform frameworks return signature-verified
// transactions (StoreKit 2 JWS, Play Billing signed receipts). ge
// writes only verified entitlements to the cache. Server-side
// validation is not provided and is genuinely not needed for paid
// non-consumable unlocks against the casual game audience — the
// JWS floor handles replay, account-binding, and bundle-ID-binding.
// Add server-side validation when shipping subscriptions or
// high-value-currency consumables.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ge::iap {

enum class Type {
    NonConsumable,            // One-shot unlock (remove ads, pro upgrade)
    Consumable,               // Re-buyable (currency packs, hint bundles)
    AutoRenewingSubscription, // Monthly/yearly. Server validation recommended.
};

// Optional per-platform SKU override for legacy / migration cases.
// When a field is non-empty, that string is sent to the platform store
// verbatim instead of the auto-prefixed `<bundle-id>.<local-id>`. Leave
// either field empty to keep auto-prefix behaviour on that platform
// (mixed mode — e.g. legacy SKUs on iOS, greenfield on Android).
struct SkuMapping {
    std::string apple;       // App Store Connect product ID; e.g. "com.legacy.app.pro"
    std::string android;     // Play Console product ID; e.g. "legacy_pro"
};

struct Product {
    std::string                 id;                          // Local ID, e.g. "pro"
    Type                        type = Type::NonConsumable;
    std::optional<SkuMapping>   sku;                         // Optional override (🎯T67)
};

struct LocalisedProduct {
    std::string id;
    std::string title;
    std::string description;
    std::string price;        // Pre-formatted with currency, e.g. "$2.99"
    std::string currency;     // ISO 4217, e.g. "USD"
};

struct Result {
    bool        ok       = false;
    std::string id;
    std::string error;        // Populated when !ok
};

using BuyCallback     = std::function<void(Result)>;
using RestoreCallback = std::function<void(Result)>;

// Register the catalogue. Call once at startup, before any owned/buy/
// products/restore call. Subsequent calls with the same products are
// a no-op; re-registering an existing ID with a different type aborts
// (assertion failure — catch in development, not at runtime in the field).
void setCatalogue(std::vector<Product> catalogue);

// True iff the product is currently entitled. Cached query — safe
// to call from render loops. Returns false before setCatalogue has
// run, or for an unregistered ID.
bool owned(const std::string& id);

// Localised product metadata for price labels in UI. Empty before
// setCatalogue has run, or while the platform store hasn't yet
// returned product info (StoreKit / Play Billing populate
// asynchronously). Returns a copy.
std::vector<LocalisedProduct> products();

// Initiate a purchase. cb fires exactly once:
//   ok = true,  id = product ID on success
//   ok = false, error = reason on failure or user cancel
// May fire synchronously (StubStore) or asynchronously (platform stores).
// Callback runs on the main thread.
void buy(const std::string& id, BuyCallback cb);

// Re-query the platform for previously-purchased non-consumables.
// Apple App Review requires a visible "Restore Purchases" button —
// route it to this function. cb fires once after the query completes;
// the entitlement cache is updated before cb runs.
void restore(RestoreCallback cb);

// Testing surface — for unit tests and the in-engine debug menu (T65.6).
// On StubStore these are authoritative. On platform stores they are
// no-ops with a warning log; the entitlement cache there is owned by
// the real store.
namespace testing {

void setOwned(const std::string& id, bool owned);
void clearAll();

} // namespace testing

// In-engine debug panel for toggling entitlements without touching a
// real store. Pure state + actions — the game renders the panel using
// its own UI primitives (ge::Sprite + lunasvg recommended) and pumps
// pointer events through onRowTap/onResetAll/onForceRestore. Compile
// out of release builds by guarding instantiation with #ifndef NDEBUG.
struct DebugPanel {
    struct Row {
        std::string id;
        Type        type;
        bool        owned;
    };

    bool visible = false;

    void show()   { visible = true;  }
    void hide()   { visible = false; }
    void toggle() { visible = !visible; }

    // Live snapshot of registered products with current owned state.
    std::vector<Row> rows() const;

    // Flip a product's owned state via testing::setOwned. The next
    // call to rows() reflects the change.
    void onRowTap(const std::string& id);

    // Wipe all entitlements via testing::clearAll.
    void onResetAll();

    // Trigger restore(). Useful for verifying the restore code path
    // on stub before sandbox accounts are wired up.
    void onForceRestore(RestoreCallback cb = {});
};

} // namespace ge::iap
