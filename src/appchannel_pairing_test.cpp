// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Spyder F2: Hello.token must come from SPYDER_APP_CHANNEL_TOKEN.
// Physical iOS/Android binds are wildcard + pairing token; Hello
// without it is rejected. Android GeActivity already lifts every
// Intent extra, including the token.

#include "appchannel_internal.h"

#include <doctest.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

using nlohmann::json;

namespace {
struct EnvGuard {
    std::string key;
    std::string prior;
    bool had = false;
    EnvGuard(const char* k, const char* value) : key(k) {
        if (const char* e = std::getenv(k)) {
            had = true;
            prior = e;
        }
        if (value == nullptr) {
            unsetenv(k);
        } else {
            REQUIRE(setenv(k, value, 1) == 0);
        }
    }
    ~EnvGuard() {
        if (had) setenv(key.c_str(), prior.c_str(), 1);
        else unsetenv(key.c_str());
    }
};
}  // namespace

TEST_CASE("hello params include SPYDER_APP_CHANNEL_TOKEN when set") {
    using ge::appchannel::detail::applyHelloPairingToken;
    using ge::appchannel::detail::resolvePairingToken;
    EnvGuard token("SPYDER_APP_CHANNEL_TOKEN", "pair-secret-1");
    CHECK(resolvePairingToken() == "pair-secret-1");

    const json params = applyHelloPairingToken({
        {"app_name", "tilt"}, {"app_version", "dev"},
    });
    CHECK(params["token"] == "pair-secret-1");
    const json rt = json::from_msgpack(json::to_msgpack(params));
    CHECK(rt["token"] == "pair-secret-1");
}

TEST_CASE("hello params omit token when SPYDER_APP_CHANNEL_TOKEN is unset") {
    using ge::appchannel::detail::applyHelloPairingToken;
    using ge::appchannel::detail::resolvePairingToken;
    EnvGuard token("SPYDER_APP_CHANNEL_TOKEN", nullptr);
    CHECK(resolvePairingToken().empty());

    const json params = applyHelloPairingToken({
        {"app_name", "tilt"}, {"app_version", "dev"},
    });
    CHECK_FALSE(params.contains("token"));
}
