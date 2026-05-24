// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/audio.h>

// Testing-only reset — declared here; defined in audio.cpp inside ge::audio.
namespace ge::audio::testing { void reset(); }

namespace {

// RAII test fixture: resets the ge::audio state machine before and after
// each test case so tests are isolated from each other.
struct Fixture {
    Fixture()  { ge::audio::testing::reset(); }
    ~Fixture() { ge::audio::testing::reset(); }
};

} // namespace

// ── FocusState ──────────────────────────────────────────────────────────────

TEST_CASE("audio: initial state is Active") {
    Fixture f;
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

// ── Background / Foreground ─────────────────────────────────────────────────

TEST_CASE("audio: onBackground → Paused; onForeground → Active") {
    Fixture f;
    ge::audio::onBackground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onForeground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

TEST_CASE("audio: nested onBackground requires matching onForeground") {
    Fixture f;
    ge::audio::onBackground();
    ge::audio::onBackground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onForeground();
    // Still backgrounded — one onForeground doesn't undo two onBackground calls.
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onForeground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

TEST_CASE("audio: spurious onForeground when Active is a no-op") {
    Fixture f;
    ge::audio::onForeground();  // depth is already 0 — clamped
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

// ── Audio focus ─────────────────────────────────────────────────────────────

TEST_CASE("audio: onAudioFocusLost → Paused; onAudioFocusGained → Active") {
    Fixture f;
    ge::audio::onAudioFocusLost();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onAudioFocusGained();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

TEST_CASE("audio: nested focus lost requires matching gained") {
    Fixture f;
    ge::audio::onAudioFocusLost();
    ge::audio::onAudioFocusLost();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onAudioFocusGained();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onAudioFocusGained();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

// ── Independent orthogonal reasons ──────────────────────────────────────────

TEST_CASE("audio: background + focus lost require both to clear") {
    Fixture f;
    ge::audio::onBackground();
    ge::audio::onAudioFocusLost();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);

    // Clear background — still silenced because focus is lost.
    ge::audio::onForeground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);

    // Clear focus — now Active.
    ge::audio::onAudioFocusGained();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

TEST_CASE("audio: focus lost + background require both to clear (reversed order)") {
    Fixture f;
    ge::audio::onAudioFocusLost();
    ge::audio::onBackground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);

    ge::audio::onAudioFocusGained();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);

    ge::audio::onForeground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

// ── Registration RAII ────────────────────────────────────────────────────────

TEST_CASE("audio: registerDevice with id=0 returns empty registration") {
    Fixture f;
    auto reg = ge::audio::registerDevice(0);
    CHECK_FALSE(static_cast<bool>(reg));
}

TEST_CASE("audio: Registration move semantics") {
    Fixture f;
    // We use device ID 0 to avoid calling SDL. The registration handle
    // is empty (id_==0) but move semantics still apply.
    auto r1 = ge::audio::registerDevice(0);
    CHECK_FALSE(static_cast<bool>(r1));

    ge::audio::Registration r2 = std::move(r1);
    CHECK_FALSE(static_cast<bool>(r1));
    CHECK_FALSE(static_cast<bool>(r2));
}

// ── pauseAll / resumeAll ─────────────────────────────────────────────────────

TEST_CASE("audio: pauseAll / resumeAll don't crash with no registered devices") {
    Fixture f;
    // No devices registered — SDL functions are never called. This is a
    // no-crash / no-assert check.
    ge::audio::pauseAll();
    ge::audio::resumeAll();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}

TEST_CASE("audio: resumeAll is suppressed while backgrounded") {
    Fixture f;
    ge::audio::onBackground();
    // resumeAll should NOT resume devices when the engine thinks we're
    // backgrounded. State stays Paused.
    ge::audio::resumeAll();
    CHECK(ge::audio::state() == ge::audio::FocusState::Paused);
    ge::audio::onForeground();
    CHECK(ge::audio::state() == ge::audio::FocusState::Active);
}
