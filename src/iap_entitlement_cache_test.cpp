// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T126 — the entitlement cache prunes revoked/cleared entitlements. This
// exercises the platform-agnostic reconciliation directly; the StoreKit /
// Play Billing stores that wrap it (iap_apple.mm, iap_android.cpp) don't
// compile on desktop, so this is where the prune contract is pinned.

#include <doctest.h>

#include "iap_entitlement_cache.h"

using ge::iap::detail::EntitlementCache;
using Set = EntitlementCache::Set;

TEST_CASE("walk prunes a revoked entitlement (owned goes false)") {
    EntitlementCache cache;
    cache.prime({"A", "B"});                 // Keychain-primed: {A, B}
    CHECK(cache.owned("A"));
    CHECK(cache.owned("B"));

    cache.beginWalk();                        // StoreKit now credits only A
    CHECK(cache.walking());
    CHECK(cache.owned("B"));                  // still true DURING the walk (frame-0 behaviour)
    cache.creditFromWalk("A");
    Set persisted = cache.finishWalk();       // clear-then-populate + persist

    CHECK(cache.owned("A"));
    CHECK_FALSE(cache.owned("B"));            // B pruned
    CHECK_FALSE(cache.walking());
    CHECK(persisted == Set{"A"});             // persisted cache == {A}, NOT {A, B}
}

TEST_CASE("an empty walk prunes everything (refund-all)") {
    EntitlementCache cache;
    cache.prime({"A", "B"});
    cache.beginWalk();
    Set persisted = cache.finishWalk();       // walk credited nothing
    CHECK_FALSE(cache.owned("A"));
    CHECK_FALSE(cache.owned("B"));
    CHECK(persisted.empty());
}

TEST_CASE("a fresh purchase credits additively, outside a walk") {
    EntitlementCache cache;
    cache.prime({"A"});
    CHECK(cache.creditPurchase("B"));         // newly added
    CHECK(cache.owned("B"));
    CHECK_FALSE(cache.creditPurchase("B"));   // already owned — no-op
    CHECK(cache.set() == Set{"A", "B"});
}

TEST_CASE("abortWalk leaves the live set untouched") {
    EntitlementCache cache;
    cache.prime({"A", "B"});
    cache.beginWalk();
    cache.creditFromWalk("A");                // partial walk...
    cache.abortWalk();                        // ...then the store's query fails
    CHECK(cache.owned("A"));
    CHECK(cache.owned("B"));                  // nothing pruned
    CHECK_FALSE(cache.walking());
}

TEST_CASE("a walk re-credits a still-valid entitlement (no spurious prune)") {
    EntitlementCache cache;
    cache.prime({"A", "B"});
    cache.beginWalk();
    cache.creditFromWalk("A");
    cache.creditFromWalk("B");                // both still credited
    Set persisted = cache.finishWalk();
    CHECK(persisted == Set{"A", "B"});
}
