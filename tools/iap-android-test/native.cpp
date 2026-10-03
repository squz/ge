// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
// Compile the production Android backend against the NDK JNI ABI, then load it
// into the host JVM. Only SDL bootstrap and logging are stubbed.
#include "../../src/iap_android.cpp"

using namespace ge::iap;
using namespace ge::iap::detail;
static std::unique_ptr<Store> store;
static jclass testClass;

extern "C" JNIEXPORT void JNICALL Java_ge_IapBridgeTest_reset(
        JNIEnv* env, jclass cls, jobject activity) {
    store.reset();
    if (!testClass) testClass = static_cast<jclass>(env->NewGlobalRef(cls));
    Java_ge_IapBridge_nativeInstall(env, cls, activity);
    store = std::make_unique<AndroidStore>();
    store->setCatalogue({
        {.id = "pro", .sku = SkuMapping{.android = "legacy_pro"}},
        {.id = "other"},
    });
}

extern "C" JNIEXPORT void JNICALL Java_ge_IapBridgeTest_buy(
        JNIEnv* env, jclass cls, jstring id, jint request) {
    auto localId = jstringToStd(env, id);
    if (request == 0) {
        store->buy(localId, {});
        return;
    }
    // Callbacks are synchronous or driven on this attached test thread.
    store->buy(localId, [env, request](Result result) {
        auto method = env->GetStaticMethodID(testClass, "onResult", "(IZLjava/lang/String;Ljava/lang/String;)V");
        auto id = env->NewStringUTF(result.id.c_str());
        auto error = env->NewStringUTF(result.error.c_str());
        env->CallStaticVoidMethod(testClass, method, request, result.ok, id, error);
        env->DeleteLocalRef(id);
        env->DeleteLocalRef(error);
    });
}

extern "C" JNIEXPORT void JNICALL Java_ge_IapBridgeTest_restore(JNIEnv*, jclass) {
    store->restore({});
}

extern "C" JNIEXPORT jboolean JNICALL Java_ge_IapBridgeTest_owned(
        JNIEnv* env, jclass, jstring id) {
    return store->owned(jstringToStd(env, id));
}

extern "C" JNIEXPORT void JNICALL Java_ge_IapBridgeTest_close(JNIEnv* env, jclass) {
    store.reset();
    env->DeleteGlobalRef(testClass);
    testClass = nullptr;
    env->DeleteGlobalRef(g_activity);
    g_activity = nullptr;
}
