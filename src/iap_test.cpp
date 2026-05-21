// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/iap.h>

#include <algorithm>

namespace iap = ge::iap;

namespace {

// Each test starts from a clean slate. The backend singleton is
// process-scoped, but testing::clearAll resets entitlement state.
// Catalogue is additive across tests, which is fine — the tests
// reference distinct IDs and don't assume an empty catalogue.
struct Reset {
    Reset() { iap::testing::clearAll(); }
};

bool listed(const std::vector<iap::LocalisedProduct>& list, const std::string& id) {
    return std::any_of(list.begin(), list.end(),
                       [&](const auto& p) { return p.id == id; });
}

} // namespace

TEST_CASE("iap: owned() is false before setCatalogue") {
    Reset r;
    CHECK_FALSE(iap::owned("never_registered"));
}

TEST_CASE("iap: setCatalogue registers products that appear in products()") {
    Reset r;
    iap::setCatalogue({
        {.id = "pro",      .type = iap::Type::NonConsumable},
        {.id = "hints_10", .type = iap::Type::Consumable},
    });

    auto list = iap::products();
    CHECK(listed(list, "pro"));
    CHECK(listed(list, "hints_10"));
}

TEST_CASE("iap: setCatalogue is idempotent for same-id same-type") {
    Reset r;
    iap::setCatalogue({{.id = "pro", .type = iap::Type::NonConsumable}});
    iap::setCatalogue({{.id = "pro", .type = iap::Type::NonConsumable}});
    // No assertion fired; products() still lists pro.
    CHECK(listed(iap::products(), "pro"));
}

TEST_CASE("iap: buy succeeds and flips owned for non-consumables") {
    Reset r;
    iap::setCatalogue({{.id = "pro", .type = iap::Type::NonConsumable}});

    iap::Result r2;
    iap::buy("pro", [&](iap::Result res) { r2 = std::move(res); });

    CHECK(r2.ok);
    CHECK(r2.id == "pro");
    CHECK(r2.error.empty());
    CHECK(iap::owned("pro"));
}

TEST_CASE("iap: consumables stay un-owned after buy (re-buyable)") {
    Reset r;
    iap::setCatalogue({{.id = "hints_10", .type = iap::Type::Consumable}});

    iap::Result r2;
    iap::buy("hints_10", [&](iap::Result res) { r2 = std::move(res); });

    CHECK(r2.ok);
    CHECK_FALSE(iap::owned("hints_10"));
}

TEST_CASE("iap: buy unknown product returns ok=false") {
    Reset r;
    iap::Result r2;
    iap::buy("never_registered_here", [&](iap::Result res) { r2 = std::move(res); });

    CHECK_FALSE(r2.ok);
    CHECK(r2.id == "never_registered_here");
    CHECK_FALSE(r2.error.empty());
}

TEST_CASE("iap: restore fires the callback with ok=true on stub") {
    Reset r;
    bool fired = false;
    iap::restore([&](iap::Result res) {
        fired = true;
        CHECK(res.ok);
    });
    CHECK(fired);
}

TEST_CASE("iap: testing::setOwned controls owned() directly") {
    Reset r;
    iap::setCatalogue({{.id = "pro", .type = iap::Type::NonConsumable}});

    CHECK_FALSE(iap::owned("pro"));
    iap::testing::setOwned("pro", true);
    CHECK(iap::owned("pro"));
    iap::testing::setOwned("pro", false);
    CHECK_FALSE(iap::owned("pro"));
}

TEST_CASE("iap: testing::clearAll wipes entitlement state") {
    Reset r;
    iap::setCatalogue({
        {.id = "pro",       .type = iap::Type::NonConsumable},
        {.id = "premium",   .type = iap::Type::NonConsumable},
    });
    iap::testing::setOwned("pro", true);
    iap::testing::setOwned("premium", true);
    CHECK(iap::owned("pro"));
    CHECK(iap::owned("premium"));

    iap::testing::clearAll();
    CHECK_FALSE(iap::owned("pro"));
    CHECK_FALSE(iap::owned("premium"));
}

TEST_CASE("iap: SkuMapping registers without breaking local-id flow (🎯T67)") {
    Reset r;
    iap::setCatalogue({
        {
            .id   = "legacy_pro",
            .type = iap::Type::NonConsumable,
            .sku  = iap::SkuMapping{
                .apple   = "com.legacy.app.pro",
                .android = "legacy_pro_v1",
            },
        },
    });

    // Local-id continuity: testing::setOwned still operates on the
    // local id, the mapping mechanic doesn't leak into the testing API.
    CHECK_FALSE(iap::owned("legacy_pro"));
    iap::testing::setOwned("legacy_pro", true);
    CHECK(iap::owned("legacy_pro"));

    // The platform SKU is *not* a local id — owned() against it
    // returns false (StubStore indexes by local id only).
    CHECK_FALSE(iap::owned("com.legacy.app.pro"));
    CHECK_FALSE(iap::owned("legacy_pro_v1"));
}

TEST_CASE("iap: mixed-mode SkuMapping (one platform overridden, one auto)") {
    Reset r;
    iap::setCatalogue({
        {
            .id   = "ios_legacy",
            .type = iap::Type::NonConsumable,
            // android field empty => auto-prefix on Android, override on iOS
            .sku  = iap::SkuMapping{.apple = "com.old.bundle.gold"},
        },
    });

    iap::testing::setOwned("ios_legacy", true);
    CHECK(iap::owned("ios_legacy"));
}

TEST_CASE("iap: buy() against StubStore is unaffected by SkuMapping presence") {
    Reset r;
    iap::setCatalogue({
        {
            .id   = "stub_legacy",
            .type = iap::Type::NonConsumable,
            .sku  = iap::SkuMapping{.apple = "com.x.y.z", .android = "stub_legacy_v2"},
        },
    });

    iap::Result res;
    iap::buy("stub_legacy", [&](iap::Result r2) { res = std::move(r2); });

    CHECK(res.ok);
    CHECK(res.id == "stub_legacy");
    CHECK(iap::owned("stub_legacy"));
}

TEST_CASE("iap::DebugPanel: rows() reflects live entitlement state") {
    Reset r;
    iap::setCatalogue({
        {.id = "panel_pro",    .type = iap::Type::NonConsumable},
        {.id = "panel_hints",  .type = iap::Type::Consumable},
    });
    iap::testing::setOwned("panel_pro", true);

    iap::DebugPanel panel;
    auto rows = panel.rows();

    auto findRow = [&](const std::string& id) {
        return std::find_if(rows.begin(), rows.end(),
                            [&](const auto& r) { return r.id == id; });
    };
    auto proRow   = findRow("panel_pro");
    auto hintsRow = findRow("panel_hints");
    REQUIRE(proRow != rows.end());
    REQUIRE(hintsRow != rows.end());
    CHECK(proRow->owned);
    CHECK_FALSE(hintsRow->owned);
}

TEST_CASE("iap::DebugPanel: onRowTap toggles owned state") {
    Reset r;
    iap::setCatalogue({{.id = "tappable", .type = iap::Type::NonConsumable}});

    iap::DebugPanel panel;
    CHECK_FALSE(iap::owned("tappable"));
    panel.onRowTap("tappable");
    CHECK(iap::owned("tappable"));
    panel.onRowTap("tappable");
    CHECK_FALSE(iap::owned("tappable"));
}

TEST_CASE("iap::DebugPanel: onResetAll clears entitlements") {
    Reset r;
    iap::setCatalogue({
        {.id = "a", .type = iap::Type::NonConsumable},
        {.id = "b", .type = iap::Type::NonConsumable},
    });
    iap::testing::setOwned("a", true);
    iap::testing::setOwned("b", true);

    iap::DebugPanel panel;
    panel.onResetAll();
    CHECK_FALSE(iap::owned("a"));
    CHECK_FALSE(iap::owned("b"));
}

TEST_CASE("iap::DebugPanel: visibility helpers") {
    iap::DebugPanel panel;
    CHECK_FALSE(panel.visible);
    panel.show();
    CHECK(panel.visible);
    panel.hide();
    CHECK_FALSE(panel.visible);
    panel.toggle();
    CHECK(panel.visible);
    panel.toggle();
    CHECK_FALSE(panel.visible);
}

TEST_CASE("iap::DebugPanel: onForceRestore fires the callback") {
    Reset r;
    bool fired = false;
    iap::DebugPanel panel;
    panel.onForceRestore([&](iap::Result res) {
        fired = true;
        CHECK(res.ok);
    });
    CHECK(fired);
}
