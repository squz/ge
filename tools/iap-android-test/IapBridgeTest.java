// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
package ge;

import android.app.Activity;
import android.text.TextUtils;
import android.util.Log;
import com.android.billingclient.api.*;
import java.util.*;
import static org.mockito.Mockito.*;
import static com.android.billingclient.api.BillingClient.BillingResponseCode.*;

// No copy of the purchase state machine: buy enters AndroidStore, calls the
// real Java bridge, and Billing listener updates return through production JNI.
public final class IapBridgeTest {
    static native void reset(Activity activity);
    static native void buy(String id, int request);
    static native boolean owned(String id);
    static native void restore();
    static native void close();
    record Result(int request, boolean ok, String id, String error) {}
    static final List<Result> results = new ArrayList<>();
    static Runnable onCallback;
    static IapBridge bridge;
    static BillingClient client;
    static BillingResult launchResult;
    static Runnable duringLaunch;
    static int launches;
    static boolean supplyProducts = true;
    static List<Purchase> restored = List.of();
    static int tests;

    public static void onResult(int request, boolean ok, String id, String error) {
        results.add(new Result(request, ok, id, error));
        // Exercise the native lock boundary even for synchronous rejection.
        owned(id);
        Runnable action = onCallback;
        onCallback = null;
        if (action != null) action.run();
    }
    static BillingResult result(int code, String message) {
        return BillingResult.newBuilder().setResponseCode(code).setDebugMessage(message).build();
    }
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message + ": " + results);
    }
    static void failed(int index, int request, String id, String error) {
        Result r = results.get(index);
        check(r.request == request && !r.ok && r.id.equals(id) && r.error.contains(error), "failed result mismatch");
        check(!r.error.isBlank(), "empty failure reason");
    }
    static void fresh(Activity activity) {
        results.clear();
        onCallback = null;
        duringLaunch = null;
        launches = 0;
        launchResult = result(OK, "");
        reset(activity);
    }
    static Purchase purchase(String sku, int state) {
        Purchase p = mock(Purchase.class);
        when(p.getProducts()).thenReturn(new ArrayList<>(List.of(sku)));
        when(p.getPurchaseState()).thenReturn(state);
        when(p.isAcknowledged()).thenReturn(true);
        when(p.getPurchaseToken()).thenReturn("token-" + sku);
        return p;
    }
    static void pass(String name) { tests++; System.out.println("PASS " + name); }

    public static void main(String[] args) {
        System.loadLibrary("iap-test");
        try (var billing = mockStatic(BillingClient.class);
             var text = mockStatic(TextUtils.class);
             var log = mockStatic(Log.class)) {
            text.when(() -> TextUtils.isEmpty(any())).thenAnswer(call -> {
                CharSequence s = call.getArgument(0);
                return s == null || s.length() == 0;
            });
            Activity activity = mock(Activity.class);
            when(activity.getPackageName()).thenReturn("test.app");
            client = mock(BillingClient.class);
            BillingClient.Builder builder = mock(BillingClient.Builder.class, RETURNS_SELF);
            billing.when(() -> BillingClient.newBuilder(activity)).thenReturn(builder);
            when(builder.setListener(any())).thenAnswer(call -> {
                bridge = (IapBridge) call.getArgument(0);
                return builder;
            });
            when(builder.build()).thenReturn(client);
            doAnswer(call -> {
                ProductDetailsResponseListener listener = call.getArgument(1);
                List<ProductDetails> products = new ArrayList<>();
                if (supplyProducts) {
                    for (String sku : List.of("legacy_pro", "test.app.other")) {
                        // Billing's real builders also read internal string
                        // metadata; absent optional fields are empty, not null.
                        ProductDetails pd = mock(ProductDetails.class, invocation ->
                                invocation.getMethod().getReturnType() == String.class
                                        ? "" : RETURNS_DEFAULTS.answer(invocation));
                        when(pd.getProductId()).thenReturn(sku);
                        when(pd.getProductType()).thenReturn(BillingClient.ProductType.INAPP);
                        when(pd.getSubscriptionOfferDetails()).thenReturn(null);
                        when(pd.getTitle()).thenReturn(sku);
                        when(pd.getDescription()).thenReturn("");
                        ProductDetails.OneTimePurchaseOfferDetails offer = mock(ProductDetails.OneTimePurchaseOfferDetails.class);
                        when(offer.getFormattedPrice()).thenReturn("$0.99");
                        when(offer.getPriceCurrencyCode()).thenReturn("USD");
                        when(pd.getOneTimePurchaseOfferDetails()).thenReturn(offer);
                        products.add(pd);
                    }
                }
                listener.onProductDetailsResponse(result(OK, ""), products);
                return null;
            }).when(client).queryProductDetailsAsync(any(), any());
            doAnswer(call -> {
                PurchasesResponseListener listener = call.getArgument(1);
                listener.onQueryPurchasesResponse(result(OK, ""), restored);
                return null;
            }).when(client).queryPurchasesAsync(any(QueryPurchasesParams.class), any());
            when(client.launchBillingFlow(any(), any())).thenAnswer(call -> {
                launches++;
                BillingResult response = launchResult;
                Runnable action = duringLaunch;
                duringLaunch = null;
                if (action != null) action.run();
                return response;
            });

            fresh(activity);
            buy("pro", 1);
            check(launches == 1 && results.isEmpty(), "launch waiting");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            check(results.size() == 1, "cancel delivered once");
            failed(0, 1, "pro", "cancelled");
            check(!owned("pro"), "cancel granted entitlement");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            check(results.size() == 1, "duplicate cancellation");
            buy("pro", 2);
            check(launches == 2 && results.size() == 1, "retry launches");
            bridge.onPurchasesUpdated(result(ERROR, "store failed"), null);
            failed(1, 2, "pro", "store failed");
            pass("cancel/null purchases, native SKU mapping, no entitlement, duplicate, retry");

            for (int code : new int[]{SERVICE_DISCONNECTED, SERVICE_UNAVAILABLE, BILLING_UNAVAILABLE,
                    ITEM_UNAVAILABLE, DEVELOPER_ERROR, ERROR, ITEM_ALREADY_OWNED, ITEM_NOT_OWNED, NETWORK_ERROR}) {
                fresh(activity);
                buy("pro", 1);
                bridge.onPurchasesUpdated(result(code, "  "), null);
                failed(0, 1, "pro", "billing response " + code);
                check(!owned("pro"), "error granted entitlement");
            }
            pass("terminal response codes get nonempty fallback errors");

            for (List<Purchase> empty : Arrays.<List<Purchase>>asList(null, List.of())) {
                fresh(activity);
                buy("pro", 1);
                bridge.onPurchasesUpdated(result(OK, ""), empty);
                failed(0, 1, "pro", "no purchases");
            }
            pass("malformed empty OK response releases pending buy");

            fresh(activity);
            buy("pro", 1);
            buy("pro", 2);
            buy("other", 3);
            failed(0, 2, "pro", "in progress");
            failed(1, 3, "other", "in progress");
            check(launches == 1, "overlap reached Play");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(2, 1, "pro", "cancelled");
            check(results.size() == 3, "overlap stole callback");
            pass("same/different SKU overlap preserves original callback");

            fresh(activity);
            buy("missing", 1);
            failed(0, 1, "missing", "unknown product");
            check(launches == 0, "unknown product launched");
            pass("unknown product callback runs outside native mutex");

            fresh(activity);
            buy("pro", 1);
            onCallback = () -> buy("pro", 2);
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            check(launches == 2 && results.size() == 1, "cancel callback retry");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "pro", "cancelled");
            pass("synchronous retry inside cancellation callback");

            fresh(activity);
            launchResult = result(ERROR, "");
            onCallback = () -> { launchResult = result(OK, ""); buy("pro", 2); };
            buy("pro", 1);
            failed(0, 1, "pro", "billing response");
            check(launches == 2 && results.size() == 1, "immediate failure retry");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "pro", "cancelled");
            pass("immediate launch failure clears state before callback");

            fresh(activity);
            launchResult = result(ERROR, "late launch return");
            duringLaunch = () -> bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            onCallback = () -> { launchResult = result(OK, ""); buy("pro", 2); };
            buy("pro", 1);
            check(launches == 2 && results.size() == 1, "old launch return clobbered retry");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "pro", "cancelled");
            pass("listener before launch returns cannot clear reentrant retry");

            supplyProducts = false;
            fresh(activity);
            buy("pro", 1);
            buy("pro", 2);
            failed(0, 1, "pro", "not yet fetched");
            failed(1, 2, "pro", "not yet fetched");
            check(launches == 0, "missing metadata launched");
            supplyProducts = true;
            pass("missing product metadata fails once and permits retry");

            fresh(activity);
            duringLaunch = () -> { throw new IllegalStateException(); };
            buy("pro", 1);
            failed(0, 1, "pro", "purchase launch failed");
            buy("pro", 2);
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "pro", "cancelled");
            pass("launch exception releases pending buy");

            fresh(activity);
            buy("pro", 0);
            buy("other", 2);
            failed(0, 2, "other", "in progress");
            check(!bridge.launchPurchase("pro"), "bridge accepted overlap");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            buy("other", 3);
            check(launches == 2 && results.size() == 1, "empty callback did not retain/release slot");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 3, "other", "cancelled");
            pass("empty callback and defensive Java overlap guard");

            fresh(activity);
            buy("pro", 1);
            bridge.onPurchasesUpdated(result(OK, ""), List.of(purchase("legacy_pro", Purchase.PurchaseState.PENDING)));
            check(results.isEmpty() && !owned("pro"), "pending became terminal/owned");
            Purchase success = purchase("legacy_pro", Purchase.PurchaseState.PURCHASED);
            onCallback = () -> buy("other", 2);
            bridge.onPurchasesUpdated(result(OK, ""), List.of(success));
            check(results.size() == 1 && results.get(0).ok && owned("pro"), "success not credited");
            check(launches == 2, "success left active state");
            bridge.onPurchasesUpdated(result(OK, ""), List.of(success));
            check(results.size() == 1, "duplicate success misrouted");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "other", "cancelled");
            check(!owned("other") && owned("pro"), "wrong entitlement after cancel");
            pass("pending, success, duplicate success, unrelated retry ownership");

            fresh(activity);
            buy("pro", 1);
            onCallback = () -> buy("pro", 2);
            success = purchase("legacy_pro", Purchase.PurchaseState.PURCHASED);
            when(success.isAcknowledged()).thenReturn(false);
            clearInvocations(client);
            bridge.onPurchasesUpdated(result(OK, ""), List.of(success, success));
            bridge.onPurchasesUpdated(result(OK, ""), List.of(success));
            verify(client, times(3)).acknowledgePurchase(any(), any());
            check(results.size() == 1 && launches == 2, "duplicate success stole same-SKU retry");
            bridge.onPurchasesUpdated(result(USER_CANCELED, ""), null);
            failed(1, 2, "pro", "cancelled");
            check(owned("pro"), "cancel revoked previous entitlement");
            restored = List.of(success);
            restore();
            check(owned("pro"), "duplicate suppression broke restore walk");
            restored = List.of();
            restore();
            check(!owned("pro"), "empty restore failed to prune");
            pass("same-SKU reentrant success retry, repeated token, restore reconciliation");
            close();
            System.out.println("Tests run: " + tests + ", Failures: 0");
        }
    }
}
