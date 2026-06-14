// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T117 — box2d → app-channel state-slice convenience. Header-only on purpose:
// libge stays physics-agnostic (it never links box2d — box2d is APP_LIBS), so
// only apps that already use box2d ever pull this in. The app owns its b2World;
// these helpers read it out into the recommended geometry schema (see
// agents-guide.md → "State slices") so a connected agent can process physics
// objects directly via spyder `app_state{...}` instead of screenshotting and
// parsing pixels.
//
// Two entry points, same body shape so they compose:
//   worldGeometry(world) — automatic: walk EVERY shape via b2World_OverlapAABB
//                          and emit one entry per body. Zero per-body code.
//   body(id, bodyId)     — curated: one labelled body, when an app wants only
//                          specific bodies or its own labels.
//
// ge formats; the app composes. Drop the result into a registerStateSlice
// getter and add whatever else the game wants (constraints, sensors, custom
// fields) alongside it:
//
//   ge::appchannel::registerStateSlice("physics",
//       [&]{ return ge::box2d::worldGeometry(state.worldId); });
//
// Shapes are reported in body-local coordinates; apply the body's `pos` + `angle`
// to place them in the world. Positions are in the world's own units (box2d
// doesn't know if that's metres or pixels — the app should add a `units` field
// if it cares).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <box2d/box2d.h>
#include <nlohmann/json.hpp>

namespace ge::box2d {

// {pos:[x,y], vel:[vx,vy], angle} for one body, in the world's own units.
inline nlohmann::json bodyState(b2BodyId body) {
    const b2Vec2 p = b2Body_GetPosition(body);
    const b2Vec2 v = b2Body_GetLinearVelocity(body);
    return {
        {"pos",   {p.x, p.y}},
        {"vel",   {v.x, v.y}},
        {"angle", b2Rot_GetAngle(b2Body_GetRotation(body))},
    };
}

// {id, pos, vel, angle} — bodyState() plus a caller-supplied label.
inline nlohmann::json body(std::string_view id, b2BodyId b) {
    nlohmann::json j = bodyState(b);
    j["id"] = std::string(id);
    return j;
}

namespace detail {

// Compact body-local outline of one shape: {type, …defining params…}.
inline nlohmann::json shapeOutline(b2ShapeId s) {
    switch (b2Shape_GetType(s)) {
        case b2_circleShape: {
            const b2Circle c = b2Shape_GetCircle(s);
            return {{"type", "circle"}, {"center", {c.center.x, c.center.y}}, {"radius", c.radius}};
        }
        case b2_capsuleShape: {
            const b2Capsule c = b2Shape_GetCapsule(s);
            return {{"type", "capsule"},
                    {"p1", {c.center1.x, c.center1.y}}, {"p2", {c.center2.x, c.center2.y}},
                    {"radius", c.radius}};
        }
        case b2_segmentShape: {
            const b2Segment seg = b2Shape_GetSegment(s);
            return {{"type", "segment"},
                    {"p1", {seg.point1.x, seg.point1.y}}, {"p2", {seg.point2.x, seg.point2.y}}};
        }
        case b2_polygonShape: {
            const b2Polygon poly = b2Shape_GetPolygon(s);
            nlohmann::json verts = nlohmann::json::array();
            for (int i = 0; i < poly.count; ++i)
                verts.push_back({poly.vertices[i].x, poly.vertices[i].y});
            return {{"type", "polygon"}, {"vertices", std::move(verts)}};
        }
        default:
            return {{"type", "other"}};
    }
}

} // namespace detail

// Walk EVERY shape in `world` (via b2World_OverlapAABB over a world-covering
// AABB — static, sleeping, and dynamic alike) and emit the recommended geometry
// schema: { "bodies": [ {id, pos, vel, angle, shapes:[outline…]} ] }, one entry
// per body, shapes grouped under their body. `id` is the box2d body name
// (b2BodyDef.name) when the app set one, else a synthesised "body_<n>". Pass the
// app's world AABB as `bounds` to clip the walk; the default covers everything.
inline nlohmann::json worldGeometry(
    b2WorldId world,
    b2AABB bounds = {{-1.0e9f, -1.0e9f}, {1.0e9f, 1.0e9f}}) {
    struct Accum {
        std::vector<std::uint64_t>                     order;   // first-seen body order
        std::unordered_map<std::uint64_t, nlohmann::json> byBody;
    } acc;

    auto collect = [](b2ShapeId shape, void* ctx) -> bool {
        auto& a = *static_cast<Accum*>(ctx);
        const b2BodyId b   = b2Shape_GetBody(shape);
        const std::uint64_t key = b2StoreBodyId(b);
        auto it = a.byBody.find(key);
        if (it == a.byBody.end()) {
            const char* name = b2Body_GetName(b);
            nlohmann::json entry = body(
                (name && name[0]) ? std::string(name)
                                  : "body_" + std::to_string(a.order.size()),
                b);
            entry["shapes"] = nlohmann::json::array();
            it = a.byBody.emplace(key, std::move(entry)).first;
            a.order.push_back(key);
        }
        it->second["shapes"].push_back(detail::shapeOutline(shape));
        return true;  // keep visiting — we want every shape
    };
    b2World_OverlapAABB(world, bounds, b2DefaultQueryFilter(), collect, &acc);

    nlohmann::json bodies = nlohmann::json::array();
    for (std::uint64_t k : acc.order) bodies.push_back(std::move(acc.byBody[k]));
    return {{"bodies", std::move(bodies)}};
}

} // namespace ge::box2d
