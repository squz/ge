// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Platform-agnostic entitlement-cache reconciliation (🎯T126). Engine-private,
// shared by the platform stores (iap_apple.mm, iap_android.cpp) and unit-tested
// directly on desktop (the platform .mm/.cpp don't compile off-device).
//
// The stores prime this from their persistent cache (Keychain on Apple) so
// owned() is correct on frame 0, then run an async "walk" of the entitlements
// the store currently credits. The walk CLEARS and REPOPULATES — so an
// entitlement the store no longer credits (revoked, refunded, Family-Sharing
// removed) is pruned, instead of surviving in the cache (and the persisted
// Keychain item, which outlives app reinstall) forever.

#pragma once

#include <string>
#include <unordered_set>
#include <utility>

namespace ge::iap::detail {

// Not thread-safe; the owning Store guards it with its own mutex.
class EntitlementCache {
public:
    using Set = std::unordered_set<std::string>;

    // Seed the live set from persistent storage so owned() is correct before
    // the async walk completes.
    void prime(Set initial) { live_ = std::move(initial); }

    bool owned(const std::string& id) const { return live_.contains(id); }
    const Set& set() const { return live_; }

    // True between beginWalk() and finishWalk()/abortWalk(). Stores that funnel
    // both walk results and fresh purchases through one callback (Android) use
    // this to route; stores with distinct callbacks (Apple) don't need it.
    bool walking() const { return walking_; }

    // Start accumulating a fresh walk. owned() keeps returning the live set
    // until finishWalk() swaps the result in — so the cache never blinks empty
    // mid-walk.
    void beginWalk() {
        pending_.clear();
        walking_ = true;
    }

    // Credit an id the store currently grants (accumulates into the walk).
    void creditFromWalk(const std::string& id) { pending_.insert(id); }

    // Clear-then-populate: replace the live set with exactly the walk result,
    // dropping anything no longer credited. Returns the new live set so the
    // caller can persist it.
    const Set& finishWalk() {
        live_ = std::move(pending_);
        pending_.clear();
        walking_ = false;
        return live_;
    }

    // Abandon an in-flight walk (e.g. the store's query failed) without
    // touching the live set.
    void abortWalk() {
        pending_.clear();
        walking_ = false;
    }

    // Additively credit a fresh purchase/transaction (outside a walk). Returns
    // true if the id was newly added (the caller should then persist).
    bool creditPurchase(const std::string& id) { return live_.insert(id).second; }

private:
    Set  live_;
    Set  pending_;
    bool walking_ = false;
};

} // namespace ge::iap::detail
