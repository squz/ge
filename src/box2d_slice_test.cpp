// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T117 — ge::box2d state-slice helpers, exercised against a tiny in-test
// b2World. (box2d is linked into the test binary via APP_LIBS.)

#include <ge/box2d_slice.h>
#include <ge/box2d_render.h>  // 🎯T131.2

#include <doctest.h>

#include <string>

using nlohmann::json;

TEST_CASE("ge::box2d::worldGeometry enumerates every body with names + shapes") {
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = {0.0f, 0.0f};
    b2WorldId world = b2CreateWorld(&wd);

    // A named dynamic circle at (1,2) moving right at 3.
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type           = b2_dynamicBody;
    bd.position       = {1.0f, 2.0f};
    bd.linearVelocity = {3.0f, 0.0f};
    bd.name           = "marble";
    b2BodyId marble = b2CreateBody(world, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    b2Circle circle = {{0.0f, 0.0f}, 0.5f};
    b2CreateCircleShape(marble, &sd, &circle);

    // An unnamed static box at the origin.
    b2BodyDef bd2 = b2DefaultBodyDef();  // default type is static
    b2BodyId wall = b2CreateBody(world, &bd2);
    b2Polygon box = b2MakeBox(1.0f, 0.5f);
    b2CreatePolygonShape(wall, &sd, &box);

    const json geo = ge::box2d::worldGeometry(world);
    REQUIRE(geo.contains("bodies"));
    const auto& bodies = geo["bodies"];
    REQUIRE(bodies.size() == 2);

    // Broad-phase walk order isn't guaranteed — find each by identity.
    const json* marbleJ = nullptr;
    const json* wallJ   = nullptr;
    for (const auto& b : bodies) {
        if (b.contains("id") && b["id"] == "marble") marbleJ = &b;
        else                                         wallJ   = &b;
    }
    REQUIRE(marbleJ);
    REQUIRE(wallJ);

    // Marble: named, pos/vel/angle read out, one circle shape (radius 0.5).
    CHECK((*marbleJ)["pos"][0].get<float>() == doctest::Approx(1.0f));
    CHECK((*marbleJ)["pos"][1].get<float>() == doctest::Approx(2.0f));
    CHECK((*marbleJ)["vel"][0].get<float>() == doctest::Approx(3.0f));
    CHECK((*marbleJ)["vel"][1].get<float>() == doctest::Approx(0.0f));
    CHECK((*marbleJ)["angle"].get<float>() == doctest::Approx(0.0f));
    REQUIRE((*marbleJ)["shapes"].size() == 1);
    CHECK((*marbleJ)["shapes"][0]["type"] == "circle");
    CHECK((*marbleJ)["shapes"][0]["radius"].get<float>() == doctest::Approx(0.5f));

    // Wall: unnamed → synthesised id, one polygon shape (4 verts).
    CHECK((*wallJ)["id"].get<std::string>().rfind("body_", 0) == 0);
    REQUIRE((*wallJ)["shapes"].size() == 1);
    CHECK((*wallJ)["shapes"][0]["type"] == "polygon");
    CHECK((*wallJ)["shapes"][0]["vertices"].size() == 4);

    b2DestroyWorld(world);
}

TEST_CASE("ge::box2d::body / bodyState produce the curated single-body form") {
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = {0.0f, 0.0f};
    b2WorldId world = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type           = b2_dynamicBody;
    bd.position       = {5.0f, -1.0f};
    bd.linearVelocity = {0.0f, 2.0f};
    b2BodyId id = b2CreateBody(world, &bd);

    const auto st = ge::box2d::bodyState(id);
    CHECK(st["pos"][0].get<float>() == doctest::Approx(5.0f));
    CHECK(st["pos"][1].get<float>() == doctest::Approx(-1.0f));
    CHECK(st["vel"][1].get<float>() == doctest::Approx(2.0f));
    CHECK_FALSE(st.contains("id"));  // bodyState is unlabelled

    const auto labelled = ge::box2d::body("hero", id);
    CHECK(labelled["id"] == "hero");
    CHECK(labelled["pos"][0].get<float>() == doctest::Approx(5.0f));

    b2DestroyWorld(world);
}

// 🎯T131.2 The box2d render-on-demand trigger: render while any body is awake,
// idle once the whole world sleeps (box2d's velocity-thresholded island sleep),
// resume when a body wakes. The signal is noise-immune by construction.
TEST_CASE("ge::box2d::renderWhileAwake tracks world wake/sleep") {
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = {0.0f, 0.0f};            // no gravity → a still body settles to sleep
    b2WorldId world = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    b2BodyId body = b2CreateBody(world, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    b2Circle c = {{0.0f, 0.0f}, 0.5f};
    b2CreateCircleShape(body, &sd, &c);

    ge::Context ctx(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::box2d::renderWhileAwake(ctx, world);
    CHECK(ctx.anyRenderTriggerActive());          // a fresh dynamic body is awake

    // Step until the still body sleeps (well past box2d's 0.5s timeToSleep).
    for (int i = 0; i < 200 && ctx.anyRenderTriggerActive(); ++i)
        b2World_Step(world, 1.0f / 60.0f, 4);
    CHECK_FALSE(ctx.anyRenderTriggerActive());    // settled → loop idles

    b2Body_SetAwake(body, true);                  // waking a body resumes rendering
    CHECK(ctx.anyRenderTriggerActive());

    b2DestroyWorld(world);
}
