// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/long_press.h>

using ge::LongPressWatcher;
using ge::PointerEvent;
using ge::Rect;
using ge::kMouseId;

namespace {

PointerEvent down(float x, float y, SDL_FingerID id = kMouseId) {
    return {.kind = PointerEvent::Down, .pos = {x, y}, .id = id};
}
PointerEvent move(float x, float y, SDL_FingerID id = kMouseId) {
    return {.kind = PointerEvent::Move, .pos = {x, y}, .id = id};
}
PointerEvent up(float x, float y, SDL_FingerID id = kMouseId) {
    return {.kind = PointerEvent::Up, .pos = {x, y}, .id = id};
}

} // namespace

TEST_CASE("LongPressWatcher: hold inside past threshold fires once") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    CHECK(w.handleEvent(down(50, 50)));
    CHECK(w.tracking);
    CHECK(fires == 0);

    // Halfway — no fire yet.
    w.update(0.5f);
    CHECK(fires == 0);

    // Crossing the threshold.
    w.update(0.6f);
    CHECK(fires == 1);

    // Further updates while still held — no refires.
    w.update(0.5f);
    w.update(0.5f);
    CHECK(fires == 1);
}

TEST_CASE("LongPressWatcher: lift before threshold doesn't fire") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    w.handleEvent(down(50, 50));
    w.update(0.3f);
    w.handleEvent(up(50, 50));
    CHECK_FALSE(w.tracking);
    CHECK(fires == 0);
}

TEST_CASE("LongPressWatcher: drag outside cancels without firing") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    w.handleEvent(down(50, 50));
    w.update(0.5f);
    w.handleEvent(move(200, 50));  // outside
    CHECK_FALSE(w.tracking);
    w.update(2.0f);                 // way past threshold, but cancelled
    CHECK(fires == 0);
}

TEST_CASE("LongPressWatcher: drag-in does NOT capture (only Down inside captures)") {
    // Discriminates LongPressWatcher from ge::Button's T62 drift-in
    // capture — long-press is "stay still and hold", drift-in capture
    // would change that contract. So Move-while-Idle is intentionally
    // not a capture event here.
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    CHECK_FALSE(w.handleEvent(move(50, 50)));   // Move while idle = no-op
    CHECK_FALSE(w.tracking);
    w.update(2.0f);
    CHECK(fires == 0);
}

TEST_CASE("LongPressWatcher: outside Down is ignored") {
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
    };
    CHECK_FALSE(w.handleEvent(down(200, 200)));
    CHECK_FALSE(w.tracking);
}

TEST_CASE("LongPressWatcher: second finger ignored while tracking first") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    CHECK(w.handleEvent(down(50, 50, /*id=*/1)));
    CHECK_FALSE(w.handleEvent(down(60, 60, /*id=*/2)));   // ignored
    CHECK(w.activeId == 1);

    // Finger 2 lifts — also ignored (not the tracked finger).
    CHECK_FALSE(w.handleEvent(up(60, 60, /*id=*/2)));
    CHECK(w.tracking);

    w.update(1.1f);
    CHECK(fires == 1);
}

TEST_CASE("LongPressWatcher: cancel during hold prevents fire") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    w.handleEvent(down(50, 50));
    w.update(0.5f);
    w.cancel();
    CHECK_FALSE(w.tracking);
    w.update(2.0f);
    CHECK(fires == 0);
}

TEST_CASE("LongPressWatcher: re-press after fire fires again on next threshold cross") {
    int fires = 0;
    LongPressWatcher w{
        .region       = Rect{0, 0, 100, 100},
        .thresholdSec = 1.0f,
        .onFire       = [&]{ ++fires; },
    };

    // First press → fires.
    w.handleEvent(down(50, 50));
    w.update(1.1f);
    CHECK(fires == 1);
    w.handleEvent(up(50, 50));
    CHECK_FALSE(w.tracking);

    // Second press → fires again.
    w.handleEvent(down(50, 50));
    w.update(1.1f);
    CHECK(fires == 2);
}
