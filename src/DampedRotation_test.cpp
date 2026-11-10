#include <doctest.h>
#include <ge/DampedRotation.h>

#include <cmath>

TEST_CASE("DampedRotation starts at identity") {
    DampedRotation rot;
    auto q = rot.orientation();
    CHECK(q.w == doctest::Approx(1.0f));
    CHECK(q.x == doctest::Approx(0.0f));
    CHECK(q.y == doctest::Approx(0.0f));
    CHECK(q.z == doctest::Approx(0.0f));
    CHECK_FALSE(rot.isMoving());
}

TEST_CASE("horizontal drag rotates around Z") {
    DampedRotation rot;
    rot.applyDrag(0.5f, 0.0f);
    auto q = rot.orientation();
    CHECK(q.w != doctest::Approx(1.0f));
    CHECK(q.z != doctest::Approx(0.0f));
    CHECK(q.x == doctest::Approx(0.0f));
}

TEST_CASE("vertical drag rotates around X") {
    DampedRotation rot;
    rot.applyDrag(0.0f, 0.5f);
    auto q = rot.orientation();
    CHECK(q.w != doctest::Approx(1.0f));
    CHECK(q.x != doctest::Approx(0.0f));
    CHECK(q.z == doctest::Approx(0.0f));
}

TEST_CASE("inertia decays to rest") {
    DampedRotation rot;
    rot.setAngularVelocity({0.0f, 0.0f, 1.0f});
    CHECK(rot.isMoving());

    for (int i = 0; i < 600; ++i) {
        rot.update(1.0f / 60.0f);
    }
    CHECK_FALSE(rot.isMoving());
}

TEST_CASE("damping is framerate-independent") {
    DampedRotation a(0.92f);
    DampedRotation b(0.92f);
    float3 vel{0.0f, 0.0f, 1.0f};
    a.setAngularVelocity(vel);
    b.setAngularVelocity(vel);

    for (int i = 0; i < 60; ++i) a.update(1.0f / 60.0f);
    for (int i = 0; i < 30; ++i) b.update(1.0f / 30.0f);

    auto qa = a.orientation();
    auto qb = b.orientation();
    CHECK(qa.w == doctest::Approx(qb.w).epsilon(0.02));
    CHECK(qa.x == doctest::Approx(qb.x).epsilon(0.02));
    CHECK(qa.y == doctest::Approx(qb.y).epsilon(0.02));
    CHECK(qa.z == doctest::Approx(qb.z).epsilon(0.02));
}

TEST_CASE("drag sequence is deterministic") {
    DampedRotation a;
    DampedRotation b;
    for (int i = 0; i < 10; ++i) {
        a.applyDrag(0.1f, 0.05f);
        b.applyDrag(0.1f, 0.05f);
    }

    auto qa = a.orientation();
    auto qb = b.orientation();
    CHECK(qa.w == doctest::Approx(qb.w));
    CHECK(qa.x == doctest::Approx(qb.x));
    CHECK(qa.y == doctest::Approx(qb.y));
    CHECK(qa.z == doctest::Approx(qb.z));
}

// 🎯T122/T123 — the camera basis parameterises the rotation axes; the default
// is the Z-up convention so the cases above (no basis arg) are unchanged.

TEST_CASE("default basis matches explicit Z-up basis") {
    DampedRotation a;
    DampedRotation b;
    a.applyDrag(0.3f, 0.2f);
    b.applyDrag(0.3f, 0.2f, 1.0f, float3{0, 0, 1}, float3{1, 0, 0});
    CHECK(a.orientation().x == doctest::Approx(b.orientation().x));
    CHECK(a.orientation().y == doctest::Approx(b.orientation().y));
    CHECK(a.orientation().z == doctest::Approx(b.orientation().z));
    CHECK(a.orientation().w == doctest::Approx(b.orientation().w));
}

TEST_CASE("Y-up basis: horizontal drag rotates around Y") {
    DampedRotation rot;
    rot.applyDrag(0.5f, 0.0f, 1.0f, float3{0, 1, 0}, float3{1, 0, 0});
    auto q = rot.orientation();
    CHECK(q.y != doctest::Approx(0.0f));
    CHECK(q.x == doctest::Approx(0.0f));
    CHECK(q.z == doctest::Approx(0.0f));
}

TEST_CASE("Y-up basis: vertical drag rotates around X") {
    DampedRotation rot;
    rot.applyDrag(0.0f, 0.5f, 1.0f, float3{0, 1, 0}, float3{1, 0, 0});
    auto q = rot.orientation();
    CHECK(q.x != doctest::Approx(0.0f));
    CHECK(q.y == doctest::Approx(0.0f));
    CHECK(q.z == doctest::Approx(0.0f));
}

TEST_CASE("velocity basis: Y-up horizontal drag yields Y-axis angular velocity") {
    DampedRotation rot;
    rot.updateVelocityFromDrag(1.0f, 0.0f, 1.0f, float3{0, 1, 0}, float3{1, 0, 0});
    auto v = rot.angularVelocity();
    CHECK(v.y != doctest::Approx(0.0f));
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.z == doctest::Approx(0.0f));
}

// 1200 px/s * sensitivity 0.01 = 12 rad/s. The release window reports that
// speed (displacement/dt), which a steady swipe's old EMA converged to.
TEST_CASE("drag velocity matches the recent sample window") {
    DampedRotation rot;
    double t = 0;
    for (int i = 0; i < 8; ++i) {
        t += 1.0 / 60.0;
        rot.updateVelocityFromDrag(1200.0f, 0.0f, 0.01f, float3{0, 0, 1}, float3{1, 0, 0}, t);
    }
    CHECK(rot.angularVelocity().z == doctest::Approx(12.0f).epsilon(0.02));
    CHECK(rot.angularVelocity().x == doctest::Approx(0.0f));
    CHECK(rot.angularVelocity().y == doctest::Approx(0.0f));
}

TEST_CASE("stationary sample a second later clears drag velocity") {
    DampedRotation rot;
    double t = 0;
    for (int i = 0; i < 8; ++i) {
        t += 1.0 / 60.0;
        rot.updateVelocityFromDrag(1200.0f, 0.0f, 0.01f, float3{0, 0, 1}, float3{1, 0, 0}, t);
    }
    t += 1.0;
    rot.updateVelocityFromDrag(0.0f, 0.0f, 0.01f, float3{0, 0, 1}, float3{1, 0, 0}, t);
    CHECK(std::fabs(rot.angularVelocity().x) < 1e-3f);
    CHECK(std::fabs(rot.angularVelocity().y) < 1e-3f);
    CHECK(std::fabs(rot.angularVelocity().z) < 1e-3f);
}
