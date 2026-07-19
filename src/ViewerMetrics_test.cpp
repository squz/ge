// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/ViewerMetrics.h>

using ge::DualSafeInsets;
using ge::SafeAreaInsets;
using ge::ViewerWindow;
using ge::fitContentRect;
using ge::mapViewerDualSafeInsets;
using ge::mapViewerSafeInsetsToContent;

TEST_CASE("fitContentRect letterboxes landscape into portrait") {
    float x, y, w, h;
    fitContentRect(1080, 1920, 4.f / 3.f, x, y, w, h);
    CHECK(w == doctest::Approx(1080.f));
    CHECK(h == doctest::Approx(1080.f * 3.f / 4.f));
    CHECK(x == doctest::Approx(0.f));
    CHECK(y == doctest::Approx((1920.f - h) * 0.5f));
}

TEST_CASE("mapViewerSafeInsets: no safe rect → zero insets") {
    ViewerWindow v;
    v.w = 1080;
    v.h = 1920;
    const SafeAreaInsets ins =
        mapViewerSafeInsetsToContent(v, 2048, 1536, 1.f);
    CHECK(ins.x0 == doctest::Approx(0.f));
    CHECK(ins.y0 == doctest::Approx(0.f));
    CHECK(ins.x1 == doctest::Approx(0.f));
    CHECK(ins.y1 == doctest::Approx(0.f));
}

TEST_CASE("mapViewerSafeInsets: side notch on landscape content") {
    ViewerWindow v;
    v.w = 2048;
    v.h = 1536;
    v.safeX = 80;
    v.safeY = 0;
    v.safeW = 2048 - 80;
    v.safeH = 1536;

    const SafeAreaInsets ins =
        mapViewerSafeInsetsToContent(v, 2048, 1536, 2.f);
    CHECK(ins.x0 == doctest::Approx(80.f / 2.f));
    CHECK(ins.y0 == doctest::Approx(0.f));
    CHECK(ins.x1 == doctest::Approx(0.f));
    CHECK(ins.y1 == doctest::Approx(0.f));
}

TEST_CASE("mapViewerSafeInsets: chrome intersects content → positive y0") {
    ViewerWindow v;
    v.w = 1000;
    v.h = 800;
    v.safeX = 0;
    v.safeY = 60;
    v.safeW = 1000;
    v.safeH = 800 - 60 - 40;

    const SafeAreaInsets ins =
        mapViewerSafeInsetsToContent(v, 1000, 800, 1.f);
    CHECK(ins.y0 == doctest::Approx(60.f));
    CHECK(ins.y1 == doctest::Approx(40.f));
    CHECK(ins.x0 == doctest::Approx(0.f));
    CHECK(ins.x1 == doctest::Approx(0.f));
}

TEST_CASE("mapViewerDualSafeInsets: draw and ui differ when both supplied") {
    // Window == content (no letterbox). Draw cutout top 20; UI also bottom 40.
    ViewerWindow v;
    v.w = 1000;
    v.h = 800;
    v.drawSafeX = 0;
    v.drawSafeY = 20;
    v.drawSafeW = 1000;
    v.drawSafeH = 780; // only top cutout
    v.safeX = 0;
    v.safeY = 20;
    v.safeW = 1000;
    v.safeH = 740; // top 20 + bottom 40

    const DualSafeInsets d =
        mapViewerDualSafeInsets(v, 1000, 800, 1.f);
    CHECK(d.draw.y0 == doctest::Approx(20.f));
    CHECK(d.draw.y1 == doctest::Approx(0.f)); // full height to bottom of content
    CHECK(d.ui.y0 == doctest::Approx(20.f));
    CHECK(d.ui.y1 == doctest::Approx(40.f));
    // Critical: draw != ui for the gesture edge.
    CHECK(d.draw.y1 != d.ui.y1);
}

TEST_CASE("mapViewerDualSafeInsets: drawSafeW==0 falls back to ui for both") {
    ViewerWindow v;
    v.w = 1000;
    v.h = 800;
    v.safeX = 0;
    v.safeY = 50;
    v.safeW = 1000;
    v.safeH = 700;
    // drawSafeW left 0

    const DualSafeInsets d =
        mapViewerDualSafeInsets(v, 1000, 800, 1.f);
    CHECK(d.draw.y0 == doctest::Approx(d.ui.y0));
    CHECK(d.draw.y1 == doctest::Approx(d.ui.y1));
    CHECK(d.ui.y0 == doctest::Approx(50.f));
    CHECK(d.ui.y1 == doctest::Approx(50.f));
}

TEST_CASE("Context drawSafeRectInPts respects setDrawSafeInsets") {
    ge::Context ctx(200, 100, ge::DeviceClass::Phone, ":memory:");
    ctx.setPixelsPerPt(2.f);
    ctx.setDrawSafeInsets({/*y0*/10.f, /*y1*/5.f, /*x0*/4.f, /*x1*/2.f});
    ctx.setUiSafeInsets({/*y0*/12.f, /*y1*/8.f, /*x0*/4.f, /*x1*/2.f});
    const ge::Rect d = ctx.drawSafeRectInPts();
    const ge::Rect u = ctx.uiSafeRectInPts();
    const ge::Rect f = ctx.fullRectInPts();
    CHECK(f.w == doctest::Approx(100.f));
    CHECK(f.h == doctest::Approx(50.f));
    CHECK(d.y == doctest::Approx(10.f));
    CHECK(u.y == doctest::Approx(12.f));
    CHECK(d.h == doctest::Approx(50.f - 10.f - 5.f));
    CHECK(u.h == doctest::Approx(50.f - 12.f - 8.f));
    CHECK(d.h > u.h); // draw larger than ui when gesture margins apply
}
