// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// StoreKit 2 worker for ge::iap (🎯T68). Compiled only on iOS / iPadOS
// / tvOS / watchOS — driven by the consumer's Xcode project; macOS
// desktop builds (Module.mk) skip this file entirely and fall back to
// the stub backend.
//
// Implements `GEStoreKit2Bridge` declared in `iap_apple_bridge.h`.
// The C++ adapter in `iap_apple.mm` owns one instance, sets a
// listener, and invokes coarse async methods. We use SK2 internally
// (Transaction.updates listener, Transaction.currentEntitlements,
// Product.purchase, AppStore.sync) and call back to the listener on
// the main actor when async work resolves.
//
// Verification policy: only `.verified` results from
// `VerificationResult<Transaction>` reach the listener. `.unverified`
// transactions are logged and dropped — they don't credit
// entitlements. This is the on-device JWS verification floor that
// motivated the migration.

import Foundation
import os.log
import StoreKit

@available(iOS 15.0, iPadOS 15.0, tvOS 15.0, watchOS 8.0, *)
@objc(GEStoreKit2BridgeImpl)
public final class GEStoreKit2BridgeImpl: NSObject, GEStoreKit2Bridge {
    @objc public weak var listener: GEStoreKit2Listener?

    private var products: [String: Product] = [:]
    private var updatesTask: Task<Void, Never>?
    private let log = OSLog(subsystem: "ge.iap", category: "storekit2")

    // MARK: - Lifecycle

    @objc public func start() {
        guard updatesTask == nil else { return }
        updatesTask = Task.detached { [weak self] in
            for await result in Transaction.updates {
                await self?.handleTransactionUpdate(result, sku: nil)
            }
        }
    }

    @objc public func stop() {
        updatesTask?.cancel()
        updatesTask = nil
    }

    // MARK: - Product catalogue

    @objc public func fetchProducts(_ skus: [String]) {
        Task { [weak self] in
            guard let self else { return }
            do {
                let fetched = try await Product.products(for: skus)
                await MainActor.run {
                    for p in fetched {
                        self.products[p.id] = p
                        let currency = p.priceFormatStyle.currencyCode
                        self.listener?.onProductFetched(p.id,
                                                        title: p.displayName,
                                                        description: p.description,
                                                        price: p.displayPrice,
                                                        currency: currency)
                    }
                    if fetched.count < skus.count {
                        let returned = Set(fetched.map(\.id))
                        for sku in skus where !returned.contains(sku) {
                            os_log("StoreKit 2 product not found: %{public}@",
                                   log: self.log, type: .info, sku)
                        }
                    }
                }
            } catch {
                os_log("Product.products failed: %{public}@",
                       log: self.log, type: .error, error.localizedDescription)
            }
        }
    }

    // MARK: - Purchase

    @objc public func purchase(_ sku: String) {
        guard let product = products[sku] else {
            listener?.onTransactionUpdate(sku,
                                          ok: false,
                                          error: "product not yet fetched from store")
            return
        }
        Task { [weak self] in
            guard let self else { return }
            do {
                let result = try await product.purchase()
                await MainActor.run {
                    switch result {
                    case .success(let verification):
                        Task { await self.handleTransactionUpdate(verification, sku: sku) }
                    case .userCancelled:
                        self.listener?.onTransactionUpdate(sku, ok: false, error: "userCancelled")
                    case .pending:
                        self.listener?.onTransactionUpdate(sku, ok: false, error: "pending")
                    @unknown default:
                        self.listener?.onTransactionUpdate(sku, ok: false, error: "unknownPurchaseResult")
                    }
                }
            } catch {
                await MainActor.run {
                    self.listener?.onTransactionUpdate(sku, ok: false, error: error.localizedDescription)
                }
            }
        }
    }

    // MARK: - Current entitlements (silent auto-restore)

    @objc public func loadCurrentEntitlements() {
        Task { [weak self] in
            guard let self else { return }
            for await result in Transaction.currentEntitlements {
                await self.handleTransactionUpdate(result, sku: nil, finishTransaction: false)
            }
            await MainActor.run {
                self.listener?.onCurrentEntitlementsFinished()
            }
        }
    }

    // MARK: - Restore (user-driven)

    @objc public func restore() {
        Task { [weak self] in
            guard let self else { return }
            do {
                try await AppStore.sync()
                await MainActor.run { self.listener?.onRestoreFinished(true, error: "") }
            } catch {
                await MainActor.run {
                    self.listener?.onRestoreFinished(false, error: error.localizedDescription)
                }
            }
        }
    }

    // MARK: - Internal

    // Routes a verified/unverified transaction result to the listener.
    // `sku` is supplied when the caller knows what they bought (post-
    // purchase); the Transaction.updates and currentEntitlements
    // paths pass nil and we derive the SKU from the transaction.
    private func handleTransactionUpdate(_ result: VerificationResult<Transaction>,
                                         sku: String?,
                                         finishTransaction: Bool = true) async {
        switch result {
        case .verified(let transaction):
            let productID = sku ?? transaction.productID
            await MainActor.run {
                if let entitlementSku = sku {
                    self.listener?.onTransactionUpdate(entitlementSku, ok: true, error: "")
                } else {
                    self.listener?.onCurrentEntitlement(productID)
                }
            }
            if finishTransaction {
                await transaction.finish()
            }
        case .unverified(let transaction, let error):
            os_log("Transaction unverified for %{public}@: %{public}@",
                   log: log, type: .error,
                   transaction.productID, error.localizedDescription)
            if let entitlementSku = sku {
                await MainActor.run {
                    self.listener?.onTransactionUpdate(entitlementSku,
                                                       ok: false,
                                                       error: "unverified: \(error.localizedDescription)")
                }
            }
            // Don't finish unverified — let Apple decide on the next launch.
        }
    }
}
