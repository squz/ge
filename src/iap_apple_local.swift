// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// LocalStore mode for ge::iap on iOS (🎯T65.4).
//
// When GE_IAP_MODE=local, iap_apple.mm instantiates this class instead of
// GEStoreKit2BridgeImpl. It starts an SKTestSession pointed at a
// StoreKit.storekit configuration file bundled in the app, then delegates
// all product-fetch and purchase calls to the same GEStoreKit2BridgeImpl
// implementation — the session intercepts StoreKit calls automatically.
//
// SKTestSession is available in the StoreKitTest framework (Xcode 12+,
// iOS 14+, tvOS 14+, watchOS 7+). On earlier OS versions the init
// gracefully falls back to nil-session (production StoreKit path, which
// will find no products and log a warning).
//
// Build wiring: add StoreKitTest.framework as an optional (weak) link in
// the consuming app's Xcode target (Build Phases → Link Binary With
// Libraries → Add → StoreKitTest.framework, set to "Optional"). Then add
// this file and iap_apple.swift to Build Phases → Compile Sources.
//
// StoreKit.storekit file: call `make ge/storekit-init` to copy the engine
// template to ios/StoreKit.storekit, then add it to the target's
// Build Phases → Copy Bundle Resources. Xcode validates the JSON against
// Apple's StoreKit Testing schema and will reject malformed files at
// build time.

import Foundation
import StoreKit

// StoreKitTest is a separate framework from StoreKit itself.
// We use a soft-import pattern via @_implementationOnly import so the
// module is absent at runtime on macOS desktop builds (which compile this
// file as a no-op stub anyway — guarded by the TARGET_OS_IPHONE check
// in iap_apple.mm).
//
// TODO: if the consuming Xcode project has not added StoreKitTest.framework
//       as a weak link, this import will fail at link time on iOS. The fix
//       is one click: Target → Build Phases → Link Binary With Libraries →
//       Add → StoreKitTest.framework → set to "Optional".
#if canImport(StoreKitTest)
import StoreKitTest
#endif

@available(iOS 15.0, iPadOS 15.0, tvOS 15.0, watchOS 8.0, *)
@objc(GEStoreKit2LocalBridgeImpl)
public final class GEStoreKit2LocalBridgeImpl: NSObject, GEStoreKit2Bridge {
    // Delegate all SK2 product/purchase work to the production bridge.
    // The SKTestSession intercepts StoreKit calls globally on this process
    // as long as it stays alive — so we only need to create the session,
    // then the same GEStoreKit2BridgeImpl can run unchanged.
    private let inner: GEStoreKit2BridgeImpl

    // Kept alive for the lifetime of this object. Releasing it ends the
    // test session and reverts to the real App Store environment.
#if canImport(StoreKitTest)
    private var session: SKTestSession?
#endif

    @objc public weak var listener: GEStoreKit2Listener? {
        get { inner.listener }
        set { inner.listener = newValue }
    }

    @objc public override init() {
        inner = GEStoreKit2BridgeImpl()
        super.init()
        startSession()
    }

    private func startSession() {
#if canImport(StoreKitTest)
        // Locate StoreKit.storekit in the app bundle. The file must be in
        // Copy Bundle Resources for this to succeed.
        guard let url = Bundle.main.url(forResource: "StoreKit", withExtension: "storekit") else {
            // Log via os_log since spdlog isn't accessible from Swift directly.
            os_log_t_warn("ge.iap",
                          "GE_IAP_MODE=local: StoreKit.storekit not found in app bundle — " +
                          "add it via Copy Bundle Resources. Falling back to production StoreKit.")
            return
        }
        do {
            let s = try SKTestSession(configurationFileURL: url)
            s.resetToDefaultState()
            s.disableDialogs = true   // Suppress purchase confirmation dialogs in tests
            s.clearTransactions()
            session = s
        } catch {
            os_log_t_warn("ge.iap",
                          "GE_IAP_MODE=local: SKTestSession init failed: \(error.localizedDescription). " +
                          "Falling back to production StoreKit.")
        }
#else
        os_log_t_warn("ge.iap",
                      "GE_IAP_MODE=local: StoreKitTest framework not available — " +
                      "link StoreKitTest.framework as Optional in Xcode target.")
#endif
    }

    // Thin delegation to inner — session intercepts StoreKit globally.
    @objc public func start()                               { inner.start() }
    @objc public func stop()                                { inner.stop() }
    @objc public func fetchProducts(_ skus: [String])      { inner.fetchProducts(skus) }
    @objc public func purchase(_ sku: String)              { inner.purchase(sku) }
    @objc public func loadCurrentEntitlements()            { inner.loadCurrentEntitlements() }
    @objc public func restore()                            { inner.restore() }
}

// Minimal os_log wrapper so we avoid importing os.log's full surface.
// os_log_t_warn maps to OS_LOG_TYPE_DEFAULT (same as spdlog warn).
private func os_log_t_warn(_ subsystem: String, _ message: String) {
    let log = OSLog(subsystem: subsystem, category: "local")
    os_log("%{public}@", log: log, type: .default, message)
}
