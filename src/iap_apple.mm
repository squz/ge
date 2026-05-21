// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Apple StoreKit backend for ge::iap (🎯T65.2).
//
// Uses StoreKit 1 (the Obj-C-compatible API) so this compiles in the
// existing Obj-C++ codebase without introducing a Swift toolchain
// dependency. StoreKit 1 still surfaces only Apple-verified
// transactions (the framework validates against Apple's servers before
// delivering); on-device JWS verification (StoreKit 2's
// VerificationResult) is a future migration when subscriptions ship.
// Apple has deprecated parts of StoreKit 1 on macOS 15 / iOS 18 in
// favor of StoreKit 2; the build emits deprecation warnings that are
// intentionally tolerated until the Swift bridging story is in place.
//
// Architecture:
//   * AppleStore (C++) implements detail::Store; owned by the dispatcher.
//   * GEStoreKitBroker (Obj-C) sits in SKPaymentQueue's observer chain
//     and SKProductsRequest's delegate slot. It keeps SKProduct*
//     references alive (strong NSMutableDictionary) and funnels
//     callbacks back to AppleStore.
//
// Bundle-ID prefixing: AppleStore takes a local ID like "pro" and
// expands to NSBundle.mainBundle.bundleIdentifier + "." + id. So
// "pro" in tiltbuggy becomes "com.squz.tiltbuggy.pro" against
// StoreKit, matching the SKU registered in App Store Connect.
//
// Threading: SKPaymentQueue / SKProductsRequest call back on the main
// thread by convention. The internal mutex protects state against any
// callers that touch AppleStore from off-main (e.g. a render thread
// calling owned()).
//
// Verification status: this file has NOT been verified on a real iOS
// device yet — that's part of 🎯T65.7's demonstration. The structure
// follows Apple's StoreKit 1 documentation; subtle bugs may surface
// at first device run.

#include "iap_internal.h"

#if defined(__APPLE__)

#import <StoreKit/StoreKit.h>

#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

@class GEStoreKitBroker;

namespace ge::iap::detail {

namespace {

// Default auto-prefix: `<bundle-id>.<local-id>` (the convention codified
// by Apple's docs). Used when Product::sku is unset or its `apple`
// field is empty (mixed-mode override on Android only).
std::string autoPrefixSku(const std::string& localId) {
    NSString* bundleId = [[NSBundle mainBundle] bundleIdentifier];
    if (!bundleId) return localId;
    return std::string([bundleId UTF8String]) + "." + localId;
}

// Resolve the App Store Connect product ID for a single Product entry.
// Respects the explicit override at `Product::sku->apple` when set,
// otherwise falls back to the auto-prefix.
std::string platformSkuFor(const Product& p) {
    if (p.sku && !p.sku->apple.empty()) return p.sku->apple;
    return autoPrefixSku(p.id);
}

} // namespace

struct AppleStore : Store {
    mutable std::mutex                                  mu;
    std::unordered_map<std::string, Product>            catalogue;   // local id -> spec
    std::unordered_set<std::string>                     entitled;    // local ids
    std::unordered_map<std::string, LocalisedProduct>   localised;   // local id -> price/title
    std::unordered_map<std::string, BuyCallback>        pendingBuys; // local id -> cb
    // 🎯T67 SKU-mapping support — populated by setCatalogue, keyed by
    // local id and (reverse) by platform SKU so transaction callbacks
    // from StoreKit can recover the local id when an explicit SKU was
    // used. Updated under `mu`.
    std::unordered_map<std::string, std::string>        platformSkuByLocal; // local -> platform
    std::unordered_map<std::string, std::string>        localBySkuPlatform; // platform -> local
    RestoreCallback                                     pendingRestore;
    GEStoreKitBroker* __strong                          broker = nil;

    AppleStore();
    ~AppleStore() override;

    void setCatalogue(std::vector<Product> next) override;
    bool owned(const std::string& id) const override;
    std::vector<LocalisedProduct> products() const override;
    void buy(const std::string& id, BuyCallback) override;
    void restore(RestoreCallback) override;

    void testingSetOwned(const std::string&, bool) override {
        SPDLOG_WARN("ge::iap::testing::setOwned has no effect against AppleStore — entitlements are owned by StoreKit");
    }
    void testingClearAll() override {
        SPDLOG_WARN("ge::iap::testing::clearAll has no effect against AppleStore — entitlements are owned by StoreKit");
    }

    // Broker callbacks (run on main thread).
    void onProductsResponse(SKProductsResponse* response);
    void onTransactionUpdate(SKPaymentTransaction* tx);
    void onRestoreFinished(NSError* err);
};

} // namespace ge::iap::detail

// ── Obj-C broker ────────────────────────────────────────────────────

@interface GEStoreKitBroker : NSObject <SKProductsRequestDelegate, SKPaymentTransactionObserver>
@property (assign, nonatomic) ge::iap::detail::AppleStore* owner;
@property (strong, nonatomic) SKProductsRequest* productsRequest;
@property (strong, nonatomic) NSMutableDictionary<NSString*, SKProduct*>* productsBySku;
@end

@implementation GEStoreKitBroker

- (instancetype)init {
    if ((self = [super init])) {
        _productsBySku = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)requestProducts:(NSSet<NSString*>*)skus {
    self.productsRequest = [[SKProductsRequest alloc] initWithProductIdentifiers:skus];
    self.productsRequest.delegate = self;
    [self.productsRequest start];
}

- (SKProduct*)productForSku:(NSString*)sku {
    return self.productsBySku[sku];
}

- (void)productsRequest:(SKProductsRequest*)request
     didReceiveResponse:(SKProductsResponse*)response {
    for (SKProduct* p in response.products) {
        self.productsBySku[p.productIdentifier] = p;
    }
    if (self.owner) self.owner->onProductsResponse(response);
}

- (void)request:(SKRequest*)request didFailWithError:(NSError*)error {
    SPDLOG_ERROR("SKProductsRequest failed: {}", [[error localizedDescription] UTF8String]);
}

- (void)paymentQueue:(SKPaymentQueue*)queue
 updatedTransactions:(NSArray<SKPaymentTransaction*>*)transactions {
    if (!self.owner) return;
    for (SKPaymentTransaction* tx in transactions) {
        self.owner->onTransactionUpdate(tx);
    }
}

- (void)paymentQueueRestoreCompletedTransactionsFinished:(SKPaymentQueue*)queue {
    if (self.owner) self.owner->onRestoreFinished(nil);
}

- (void)paymentQueue:(SKPaymentQueue*)queue
restoreCompletedTransactionsFailedWithError:(NSError*)error {
    if (self.owner) self.owner->onRestoreFinished(error);
}

@end

// ── AppleStore method bodies ────────────────────────────────────────

namespace ge::iap::detail {

AppleStore::AppleStore() {
    broker = [[GEStoreKitBroker alloc] init];
    broker.owner = this;
    [[SKPaymentQueue defaultQueue] addTransactionObserver:broker];
}

AppleStore::~AppleStore() {
    [[SKPaymentQueue defaultQueue] removeTransactionObserver:broker];
    broker.owner = nullptr;
    broker = nil;
}

void AppleStore::setCatalogue(std::vector<Product> next) {
    NSMutableSet<NSString*>* skus = [NSMutableSet set];
    {
        std::lock_guard lock(mu);
        for (const auto& p : next) {
            catalogue.try_emplace(p.id, p);
            std::string platformId = platformSkuFor(p);
            platformSkuByLocal[p.id]     = platformId;
            localBySkuPlatform[platformId] = p.id;
            NSString* sku = [NSString stringWithUTF8String:platformId.c_str()];
            [skus addObject:sku];
        }
    }
    [broker requestProducts:skus];
}

bool AppleStore::owned(const std::string& id) const {
    std::lock_guard lock(mu);
    return entitled.contains(id);
}

std::vector<LocalisedProduct> AppleStore::products() const {
    std::lock_guard lock(mu);
    std::vector<LocalisedProduct> out;
    out.reserve(localised.size());
    for (const auto& [id, lp] : localised) out.push_back(lp);
    return out;
}

void AppleStore::buy(const std::string& id, BuyCallback cb) {
    std::string platformId;
    {
        std::lock_guard lock(mu);
        auto it = platformSkuByLocal.find(id);
        if (it == platformSkuByLocal.end()) {
            Result r{.ok = false, .id = id, .error = "unknown product"};
            if (cb) cb(std::move(r));
            return;
        }
        platformId = it->second;
    }
    NSString* sku = [NSString stringWithUTF8String:platformId.c_str()];
    SKProduct* product = [broker productForSku:sku];
    {
        std::lock_guard lock(mu);
        if (!product) {
            Result r{.ok = false, .id = id, .error = "product not yet fetched from store"};
            if (cb) cb(std::move(r));
            return;
        }
        pendingBuys[id] = std::move(cb);
    }
    SKPayment* payment = [SKPayment paymentWithProduct:product];
    [[SKPaymentQueue defaultQueue] addPayment:payment];
}

void AppleStore::restore(RestoreCallback cb) {
    {
        std::lock_guard lock(mu);
        pendingRestore = std::move(cb);
    }
    [[SKPaymentQueue defaultQueue] restoreCompletedTransactions];
}

void AppleStore::onProductsResponse(SKProductsResponse* response) {
    std::lock_guard lock(mu);
    for (SKProduct* p in response.products) {
        std::string sku    = [p.productIdentifier UTF8String];
        auto it = localBySkuPlatform.find(sku);
        if (it == localBySkuPlatform.end()) {
            SPDLOG_WARN("StoreKit responded with unmapped product identifier: {}", sku);
            continue;
        }
        std::string localId = it->second;

        NSNumberFormatter* fmt = [[NSNumberFormatter alloc] init];
        fmt.numberStyle = NSNumberFormatterCurrencyStyle;
        fmt.locale = p.priceLocale;
        NSString* priceStr = [fmt stringFromNumber:p.price];

        NSString* currencyCode = [p.priceLocale objectForKey:NSLocaleCurrencyCode];

        localised[localId] = {
            .id          = localId,
            .title       = [p.localizedTitle UTF8String],
            .description = [p.localizedDescription UTF8String],
            .price       = priceStr ? [priceStr UTF8String] : "",
            .currency    = currencyCode ? [currencyCode UTF8String] : "",
        };
    }
    for (NSString* invalid in response.invalidProductIdentifiers) {
        SPDLOG_WARN("StoreKit reported invalid product identifier: {}", [invalid UTF8String]);
    }
}

void AppleStore::onTransactionUpdate(SKPaymentTransaction* tx) {
    std::string sku    = [tx.payment.productIdentifier UTF8String];
    std::string localId;
    bool isConsumable = false;
    {
        std::lock_guard lock(mu);
        auto skuIt = localBySkuPlatform.find(sku);
        if (skuIt == localBySkuPlatform.end()) {
            // Unmapped transaction — possible if Apple delivers a
            // leftover transaction from an earlier session whose
            // catalogue we no longer have. Finish it so StoreKit
            // doesn't keep redelivering, but skip our callback path.
            SPDLOG_WARN("Transaction for unmapped product identifier: {}", sku);
            [[SKPaymentQueue defaultQueue] finishTransaction:tx];
            return;
        }
        localId = skuIt->second;
        auto pIt = catalogue.find(localId);
        if (pIt != catalogue.end()) isConsumable = pIt->second.type == Type::Consumable;
    }

    BuyCallback cb;
    switch (tx.transactionState) {
        case SKPaymentTransactionStatePurchased:
        case SKPaymentTransactionStateRestored: {
            {
                std::lock_guard lock(mu);
                if (!isConsumable) entitled.insert(localId);
                auto cbIt = pendingBuys.find(localId);
                if (cbIt != pendingBuys.end()) {
                    cb = std::move(cbIt->second);
                    pendingBuys.erase(cbIt);
                }
            }
            if (cb) cb({.ok = true, .id = localId, .error = {}});
            [[SKPaymentQueue defaultQueue] finishTransaction:tx];
            break;
        }
        case SKPaymentTransactionStateFailed: {
            std::string err = tx.error ? [[tx.error localizedDescription] UTF8String] : "purchase failed";
            {
                std::lock_guard lock(mu);
                auto cbIt = pendingBuys.find(localId);
                if (cbIt != pendingBuys.end()) {
                    cb = std::move(cbIt->second);
                    pendingBuys.erase(cbIt);
                }
            }
            if (cb) cb({.ok = false, .id = localId, .error = std::move(err)});
            [[SKPaymentQueue defaultQueue] finishTransaction:tx];
            break;
        }
        case SKPaymentTransactionStatePurchasing:
        case SKPaymentTransactionStateDeferred:
            // In-flight; wait for terminal state.
            break;
    }
}

void AppleStore::onRestoreFinished(NSError* err) {
    RestoreCallback cb;
    {
        std::lock_guard lock(mu);
        cb = std::move(pendingRestore);
        pendingRestore = {};
    }
    if (!cb) return;
    if (err) {
        cb({.ok = false, .id = {}, .error = [[err localizedDescription] UTF8String]});
    } else {
        cb({.ok = true, .id = {}, .error = {}});
    }
}

std::unique_ptr<Store> makePlatformStore() {
    return std::make_unique<AppleStore>();
}

} // namespace ge::iap::detail

#endif // defined(__APPLE__)
