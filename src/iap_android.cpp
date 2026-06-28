// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Android Play Billing 7+ backend for ge::iap (🎯T65.3).
//
// JNI bridge to android-shared/src/main/java/ge/IapBridge.java. The
// Java side owns BillingClient + the PurchasesUpdatedListener; this
// file translates between the C++ Store interface and JNI calls,
// keeping state (catalogue, entitlements, pending callbacks) in
// pure C++.
//
// Bundle-ID prefixing: same as Apple — local ID "pro" becomes
// "com.squz.tiltbuggy.pro" against Play Billing, matching the SKU
// registered in Play Console.
//
// Threading: BillingClient callbacks run on whichever thread
// BillingClient picks (typically a background HandlerThread). All
// shared state is protected by a coarse mutex. JNI calls into Java
// take whichever thread the caller is on; we attach via JavaVM if
// necessary.
//
// Verification status: this file has NOT been verified on a real
// Android device yet — that's part of 🎯T65.7's demonstration. The
// flow follows Google's Play Billing 7 documentation; subtle bugs
// may surface at first device run (especially around acknowledge
// semantics, PENDING handling, and the JNI attach lifecycle).

#include "iap_internal.h"
#include "iap_entitlement_cache.h"

#if defined(__ANDROID__)

#include <spdlog/spdlog.h>

#include <SDL3/SDL_system.h>

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ge::iap::detail {

namespace {

JavaVM* g_jvm = nullptr;
jobject g_activity = nullptr;   // global ref, set by app at startup

// String formed by prefixing local id with the activity's package name.
std::string platformSku(JNIEnv* env, const std::string& localId);
std::string localIdFromSku(JNIEnv* env, const std::string& sku);

// JNIEnv attach helper — Play Billing callbacks may come on a thread
// that isn't attached to the JVM.
struct ScopedEnv {
    JNIEnv* env  = nullptr;
    bool    detach = false;

    ScopedEnv() {
        if (!g_jvm) return;
        int s = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (s == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
        }
    }
    ~ScopedEnv() {
        if (detach && g_jvm) g_jvm->DetachCurrentThread();
    }
};

} // namespace

struct AndroidStore : Store {
    mutable std::mutex                                  mu;
    std::unordered_map<std::string, Product>            catalogue;
    // 🎯T65.5: Android EncryptedSharedPreferences entitlement cache —
    // deferred. T69 (multimaze2 iOS App Store v1.0) is iOS-only; the
    // Android equivalent of the iOS Keychain priming (see iap_apple.mm)
    // lands when an Android consumer game ships. The pattern is the
    // same: prime `entitled` from a JNI call to GetEntitlementCache on
    // construction; persist via PutEntitlementCache after every
    // successful purchase ack and after the launch reconciliation
    // completes. Until then, owned() returns false for the first ~1 s
    // after Android process launch (BillingClient queryPurchases async
    // latency).
    EntitlementCache                                    cache;   // 🎯T126 (replaces the raw `entitled` set)
    std::unordered_map<std::string, LocalisedProduct>   localised;
    std::unordered_map<std::string, BuyCallback>        pendingBuys;
    // 🎯T67 SKU-mapping support — same shape as AppleStore. Populated
    // by setCatalogue, consulted on buy() (forward) and on
    // nativeOnProductFetched / nativeOnPurchaseUpdate (reverse).
    std::unordered_map<std::string, std::string>        platformSkuByLocal; // local -> platform
    std::unordered_map<std::string, std::string>        localBySkuPlatform; // platform -> local
    RestoreCallback                                     pendingRestore;
    jobject                                             bridge = nullptr;   // global ref

    AndroidStore();
    ~AndroidStore() override;

    void setCatalogue(std::vector<Product>) override;
    bool owned(const std::string&) const override;
    std::vector<LocalisedProduct> products() const override;
    void buy(const std::string&, BuyCallback) override;
    void restore(RestoreCallback) override;

    void testingSetOwned(const std::string&, bool) override {
        SPDLOG_WARN("ge::iap::testing::setOwned has no effect against AndroidStore — entitlements are owned by Play Billing");
    }
    void testingClearAll() override {
        SPDLOG_WARN("ge::iap::testing::clearAll has no effect against AndroidStore — entitlements are owned by Play Billing");
    }

    // Called from JNI hooks.
    void onProductFetched(const std::string& localId, LocalisedProduct lp);
    void onPurchaseUpdate(const std::string& localId, bool ok, const std::string& error);
    void onRestoreFinished(bool ok, const std::string& error);
};

namespace {

AndroidStore* g_instance = nullptr;   // set by AndroidStore ctor, cleared by dtor

std::string jstringToStd(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}

std::string platformSku(JNIEnv* env, const std::string& localId) {
    // Activity.getPackageName() yields e.g. "com.squz.tiltbuggy".
    if (!g_activity) return localId;
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "getPackageName", "()Ljava/lang/String;");
    jstring pkg = static_cast<jstring>(env->CallObjectMethod(g_activity, m));
    std::string base = jstringToStd(env, pkg);
    env->DeleteLocalRef(pkg);
    env->DeleteLocalRef(cls);
    return base + "." + localId;
}

std::string localIdFromSku(JNIEnv* env, const std::string& sku) {
    if (!g_activity) return sku;
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "getPackageName", "()Ljava/lang/String;");
    jstring pkg = static_cast<jstring>(env->CallObjectMethod(g_activity, m));
    std::string base = jstringToStd(env, pkg);
    env->DeleteLocalRef(pkg);
    env->DeleteLocalRef(cls);
    const std::string prefix = base + ".";
    if (sku.size() > prefix.size() && sku.compare(0, prefix.size(), prefix) == 0) {
        return sku.substr(prefix.size());
    }
    return sku;
}

} // namespace

AndroidStore::AndroidStore() {
    // Bootstrap g_jvm + g_activity from SDL's Android bridge so consumers
    // don't need a separate JNI_OnLoad hook. SDL_GetAndroidJNIEnv() and
    // SDL_GetAndroidActivity() are usable any time after SDL_Init runs
    // (which it has by the time ge::run reaches its render-loop body).
    if (!g_jvm) {
        JNIEnv* bootstrapEnv = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        if (bootstrapEnv) bootstrapEnv->GetJavaVM(&g_jvm);
    }
    if (!g_activity) {
        jobject local = static_cast<jobject>(SDL_GetAndroidActivity());
        if (local) {
            ScopedEnv tmp;
            if (tmp.env) {
                g_activity = tmp.env->NewGlobalRef(local);
                tmp.env->DeleteLocalRef(local);
            }
        }
    }

    ScopedEnv env;
    if (!env.env || !g_activity) {
        SPDLOG_ERROR("AndroidStore: SDL Android bridge unavailable; iap calls will no-op");
        return;
    }
    jclass cls = env.env->FindClass("ge/IapBridge");
    if (!cls) {
        SPDLOG_ERROR("AndroidStore: ge.IapBridge class not found");
        return;
    }
    jmethodID ctor = env.env->GetMethodID(cls, "<init>", "(Landroid/app/Activity;)V");
    jobject local = env.env->NewObject(cls, ctor, g_activity);
    bridge = env.env->NewGlobalRef(local);
    env.env->DeleteLocalRef(local);
    env.env->DeleteLocalRef(cls);
    g_instance = this;
}

AndroidStore::~AndroidStore() {
    ScopedEnv env;
    if (env.env && bridge) {
        jclass cls = env.env->GetObjectClass(bridge);
        jmethodID shutdown = env.env->GetMethodID(cls, "shutdown", "()V");
        env.env->CallVoidMethod(bridge, shutdown);
        env.env->DeleteGlobalRef(bridge);
        env.env->DeleteLocalRef(cls);
    }
    g_instance = nullptr;
}

void AndroidStore::setCatalogue(std::vector<Product> next) {
    ScopedEnv env;
    if (!env.env || !bridge) return;

    std::vector<std::string> skus;
    std::vector<jboolean>    subs;
    {
        std::lock_guard lock(mu);
        for (const auto& p : next) {
            catalogue.try_emplace(p.id, p);
            // 🎯T67: explicit override on Android takes precedence;
            // otherwise auto-prefix via Activity.getPackageName().
            std::string platformId = (p.sku && !p.sku->android.empty())
                ? p.sku->android
                : platformSku(env.env, p.id);
            platformSkuByLocal[p.id]       = platformId;
            localBySkuPlatform[platformId] = p.id;
            skus.push_back(platformId);
            subs.push_back(p.type == Type::AutoRenewingSubscription ? JNI_TRUE : JNI_FALSE);
        }
    }

    jclass strCls = env.env->FindClass("java/lang/String");
    jobjectArray skuArr = env.env->NewObjectArray(skus.size(), strCls, nullptr);
    for (size_t i = 0; i < skus.size(); ++i) {
        jstring s = env.env->NewStringUTF(skus[i].c_str());
        env.env->SetObjectArrayElement(skuArr, i, s);
        env.env->DeleteLocalRef(s);
    }
    jbooleanArray subArr = env.env->NewBooleanArray(subs.size());
    env.env->SetBooleanArrayRegion(subArr, 0, subs.size(), subs.data());

    jclass bridgeCls = env.env->GetObjectClass(bridge);
    jmethodID m = env.env->GetMethodID(bridgeCls, "querySkus", "([Ljava/lang/String;[Z)V");
    env.env->CallVoidMethod(bridge, m, skuArr, subArr);

    env.env->DeleteLocalRef(bridgeCls);
    env.env->DeleteLocalRef(subArr);
    env.env->DeleteLocalRef(skuArr);
    env.env->DeleteLocalRef(strCls);
}

bool AndroidStore::owned(const std::string& id) const {
    std::lock_guard lock(mu);
    return cache.owned(id);
}

std::vector<LocalisedProduct> AndroidStore::products() const {
    std::lock_guard lock(mu);
    std::vector<LocalisedProduct> out;
    out.reserve(localised.size());
    for (const auto& [id, lp] : localised) out.push_back(lp);
    return out;
}

void AndroidStore::buy(const std::string& id, BuyCallback cb) {
    ScopedEnv env;
    if (!env.env || !bridge) {
        if (cb) cb({.ok = false, .id = id, .error = "JVM not available"});
        return;
    }

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
        pendingBuys[id] = std::move(cb);
    }

    jstring sku = env.env->NewStringUTF(platformId.c_str());
    jclass bridgeCls = env.env->GetObjectClass(bridge);
    jmethodID m = env.env->GetMethodID(bridgeCls, "launchPurchase", "(Ljava/lang/String;)Z");
    env.env->CallBooleanMethod(bridge, m, sku);
    env.env->DeleteLocalRef(bridgeCls);
    env.env->DeleteLocalRef(sku);
}

void AndroidStore::restore(RestoreCallback cb) {
    ScopedEnv env;
    if (!env.env || !bridge) {
        if (cb) cb({.ok = false, .id = {}, .error = "JVM not available"});
        return;
    }
    {
        std::lock_guard lock(mu);
        pendingRestore = std::move(cb);
        cache.beginWalk();   // 🎯T126: clear-then-populate so revoked purchases are pruned
    }
    jclass bridgeCls = env.env->GetObjectClass(bridge);
    jmethodID m = env.env->GetMethodID(bridgeCls, "queryPurchases", "()V");
    env.env->CallVoidMethod(bridge, m);
    env.env->DeleteLocalRef(bridgeCls);
}

void AndroidStore::onProductFetched(const std::string& localId, LocalisedProduct lp) {
    std::lock_guard lock(mu);
    localised[localId] = std::move(lp);
}

void AndroidStore::onPurchaseUpdate(const std::string& localId, bool ok, const std::string& error) {
    BuyCallback cb;
    bool isConsumable = false;
    {
        std::lock_guard lock(mu);
        auto pIt = catalogue.find(localId);
        if (pIt != catalogue.end()) isConsumable = pIt->second.type == Type::Consumable;
        // 🎯T126: during a queryPurchases walk (restore), accumulate so the
        // finish can clear-then-populate; a fresh purchase credits directly.
        if (ok && !isConsumable) {
            if (cache.walking()) cache.creditFromWalk(localId);
            else                 cache.creditPurchase(localId);
        }
        auto cbIt = pendingBuys.find(localId);
        if (cbIt != pendingBuys.end()) {
            cb = std::move(cbIt->second);
            pendingBuys.erase(cbIt);
        }
    }
    if (cb) cb({.ok = ok, .id = localId, .error = error});
}

void AndroidStore::onRestoreFinished(bool ok, const std::string& error) {
    RestoreCallback cb;
    {
        std::lock_guard lock(mu);
        cb = std::move(pendingRestore);
        pendingRestore = {};
        // 🎯T126: a successful queryPurchases walk swaps the freshly-queried
        // set in (pruning revoked purchases); a failed one is abandoned.
        if (cache.walking()) {
            if (ok) cache.finishWalk();
            else    cache.abortWalk();
        }
    }
    if (cb) cb({.ok = ok, .id = {}, .error = error});
}

// ── AndroidLocalStore ──────────────────────────────────────────────────────
//
// GE_IAP_MODE=local on Android. Overrides AndroidStore::setCatalogue to
// substitute the reserved Play Billing test SKUs:
//
//   android.test.purchased        — always succeeds
//   android.test.canceled         — always fails (user cancelled)
//   android.test.item_unavailable — product unavailable
//   android.test.refunded         — immediately refunded
//
// All products map to android.test.purchased by default (the happy-path
// test route). A product can override this by setting:
//
//   SkuMapping.android = "android.test.canceled"    // test the failure path
//   SkuMapping.android = "android.test.item_unavailable"
//
// The local-id → test-sku mapping preserves all callback routing so
// buy("pro", cb) still fires cb with id="pro".
//
// Verification note: Play Billing will not return real product metadata for
// android.test.* SKUs — onProductFetched() will not be called. products()
// returns synthetic entries (same as StubStore) so the UI can render
// price labels during local-mode iteration.

struct AndroidLocalStore : AndroidStore {
    // Synthetic localised products (no Play Billing metadata available for
    // android.test.* SKUs — Play Billing does not return product info for them).
    std::unordered_map<std::string, LocalisedProduct> syntheticLocalised;

    void setCatalogue(std::vector<Product> next) override {
        // Remap platform SKUs to android.test.* before handing off.
        std::vector<Product> remapped = next;
        for (auto& p : remapped) {
            // If the product already has an explicit android.test.* SKU
            // override, respect it. Otherwise map to android.test.purchased.
            if (p.sku && !p.sku->android.empty() &&
                    p.sku->android.rfind("android.test.", 0) == 0) {
                // Already a test SKU — leave as-is.
            } else {
                // Force to android.test.purchased (happy path).
                if (!p.sku) p.sku = SkuMapping{};
                p.sku->android = "android.test.purchased";
            }
        }

        // Populate synthetic localised entries before calling through —
        // Play Billing won't return product info for android.test.* SKUs.
        {
            std::lock_guard lock(mu);
            for (const auto& p : next) {
                syntheticLocalised[p.id] = {
                    .id          = p.id,
                    .title       = p.id,
                    .description = "(local) " + p.id,
                    .price       = "$0.99",
                    .currency    = "USD",
                };
            }
        }

        AndroidStore::setCatalogue(std::move(remapped));
    }

    std::vector<LocalisedProduct> products() const override {
        // Return synthetic entries — Play Billing won't respond for test SKUs.
        std::lock_guard lock(mu);
        std::vector<LocalisedProduct> out;
        out.reserve(syntheticLocalised.size());
        for (const auto& [id, lp] : syntheticLocalised) out.push_back(lp);
        return out;
    }
};

std::unique_ptr<Store> makePlatformStore() {
    const char* envMode = std::getenv("GE_IAP_MODE");
    if (envMode && std::string(envMode) == "local") {
        SPDLOG_INFO("GE_IAP_MODE=local: AndroidLocalStore active — buy() routes to android.test.purchased");
        return std::make_unique<AndroidLocalStore>();
    }
    return std::make_unique<AndroidStore>();
}

} // namespace ge::iap::detail

// ── JNI exports ─────────────────────────────────────────────────────

extern "C" {

// Called from the consuming app's JNI_OnLoad (after the existing
// GeActivity init wiring) to publish the JavaVM + activity object
// references that AndroidStore needs.
JNIEXPORT void JNICALL Java_ge_IapBridge_nativeInstall(JNIEnv* env, jclass, jobject activity) {
    env->GetJavaVM(&ge::iap::detail::g_jvm);
    if (ge::iap::detail::g_activity) env->DeleteGlobalRef(ge::iap::detail::g_activity);
    ge::iap::detail::g_activity = env->NewGlobalRef(activity);
}

JNIEXPORT void JNICALL Java_ge_IapBridge_nativeOnBillingReady(JNIEnv*, jclass) {
    // Currently a no-op marker; AndroidStore lazy-queries on first
    // setCatalogue. T65.5 will use this to kick off the initial
    // queryPurchases() that rebuilds the entitlement cache.
}

JNIEXPORT void JNICALL Java_ge_IapBridge_nativeOnProductFetched(
        JNIEnv* env, jclass,
        jstring sku, jstring title, jstring desc, jstring price, jstring currency) {
    using namespace ge::iap;
    using namespace ge::iap::detail;
    if (!g_instance) return;
    auto j2s = [&](jstring s) -> std::string {
        if (!s) return {};
        const char* c = env->GetStringUTFChars(s, nullptr);
        std::string out = c ? c : "";
        env->ReleaseStringUTFChars(s, c);
        return out;
    };
    std::string skuStr = j2s(sku);
    std::string localId;
    {
        std::lock_guard lock(g_instance->mu);
        auto it = g_instance->localBySkuPlatform.find(skuStr);
        if (it != g_instance->localBySkuPlatform.end()) {
            localId = it->second;
        }
    }
    if (localId.empty()) {
        SPDLOG_WARN("Play Billing responded with unmapped product identifier: {}", skuStr);
        return;
    }
    LocalisedProduct lp{
        .id          = localId,
        .title       = j2s(title),
        .description = j2s(desc),
        .price       = j2s(price),
        .currency    = j2s(currency),
    };
    g_instance->onProductFetched(localId, std::move(lp));
}

JNIEXPORT void JNICALL Java_ge_IapBridge_nativeOnPurchaseUpdate(
        JNIEnv* env, jclass, jstring sku, jboolean ok, jstring error) {
    using namespace ge::iap::detail;
    if (!g_instance) return;
    const char* skuC = env->GetStringUTFChars(sku, nullptr);
    std::string skuStr = skuC ? skuC : "";
    env->ReleaseStringUTFChars(sku, skuC);
    std::string localId;
    {
        std::lock_guard lock(g_instance->mu);
        auto it = g_instance->localBySkuPlatform.find(skuStr);
        if (it != g_instance->localBySkuPlatform.end()) {
            localId = it->second;
        }
    }
    if (localId.empty()) {
        SPDLOG_WARN("Play Billing purchase update for unmapped product identifier: {}", skuStr);
        return;
    }
    std::string errStr;
    if (error) {
        const char* c = env->GetStringUTFChars(error, nullptr);
        errStr = c ? c : "";
        env->ReleaseStringUTFChars(error, c);
    }
    g_instance->onPurchaseUpdate(localId, ok == JNI_TRUE, errStr);
}

JNIEXPORT void JNICALL Java_ge_IapBridge_nativeOnRestoreFinished(
        JNIEnv* env, jclass, jboolean ok, jstring error) {
    using namespace ge::iap::detail;
    if (!g_instance) return;
    std::string errStr;
    if (error) {
        const char* c = env->GetStringUTFChars(error, nullptr);
        errStr = c ? c : "";
        env->ReleaseStringUTFChars(error, c);
    }
    g_instance->onRestoreFinished(ok == JNI_TRUE, errStr);
}

} // extern "C"

#endif // defined(__ANDROID__)
