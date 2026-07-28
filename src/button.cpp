// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/button.h>

#include <ge/SessionHost.h>  // T175.5 session registry

#include <algorithm>
#include <unordered_map>

namespace ge {

namespace {

// T175.5 Game-thread-only registries of Buttons that may appear in
// hit_targets, keyed by session id (0 = process defaults, visible to
// every session — the Context-less publish). Entries for sessions that
// have died are pruned lazily on read, closing the dangling-Button*
// window a process-global registry left open.
std::unordered_map<uint32_t, std::vector<Button*>>& hitTargetRegistries() {
    static std::unordered_map<uint32_t, std::vector<Button*>> regs;
    return regs;
}

uint32_t lenientSessionId() {
    const auto live = liveSessionIds();
    return live.size() == 1 ? live.front() : 0u;
}

void pruneDeadSessions() {
    auto& regs = hitTargetRegistries();
    const auto live = liveSessionIds();
    for (auto it = regs.begin(); it != regs.end();) {
        const uint32_t id = it->first;
        const bool alive = id == 0 ||
            std::find(live.begin(), live.end(), id) != live.end();
        it = alive ? std::next(it) : regs.erase(it);
    }
}

void publishTo(uint32_t sid, Button* b) {
    if (!b) return;
    auto& reg = hitTargetRegistries()[sid];
    if (std::find(reg.begin(), reg.end(), b) != reg.end()) return;
    reg.push_back(b);
}

} // namespace

void publishHitTarget(Button* b) { publishTo(lenientSessionId(), b); }

void publishHitTarget(const Context& ctx, Button* b) {
    publishTo(ctx.sessionId(), b);
}

void unpublishHitTarget(Button* b) {
    if (!b) return;
    // Remove from every session's registry — a Button must never dangle,
    // whichever session (or default) it was published under.
    for (auto& kv : hitTargetRegistries()) {
        auto& reg = kv.second;
        reg.erase(std::remove(reg.begin(), reg.end(), b), reg.end());
    }
}

std::vector<Button*> publishedHitTargets() {
    return publishedHitTargets(lenientSessionId());
}

std::vector<Button*> publishedHitTargets(uint32_t sessionId) {
    pruneDeadSessions();
    auto& regs = hitTargetRegistries();
    std::vector<Button*> out;
    if (auto d = regs.find(0); d != regs.end())
        out.insert(out.end(), d->second.begin(), d->second.end());
    if (sessionId != 0) {
        if (auto s = regs.find(sessionId); s != regs.end())
            for (Button* b : s->second)
                if (std::find(out.begin(), out.end(), b) == out.end())
                    out.push_back(b);
    }
    return out;
}

bool Button::handleEvent(const PointerEvent& ev) {
    // 🎯T131.3 Wrap the state machine so any visible-state (phase) transition
    // fires onRedraw exactly once — the single point that guarantees a
    // render-on-demand frame for the depress / release. The IIFE gives the
    // existing switch a single exit without changing its logic.
    const Phase before = phase;
    const bool consumed = [&]() -> bool {
        const bool inside = hitTest ? hitTest(ev.pos) : false;

        switch (ev.kind) {
        case PointerEvent::Down:
            // Drop subsequent presses while already tracking — single-touch
            // by design (see header docs).
            if (phase != Idle) return false;
            if (!inside) return false;
            activeId = ev.id;
            phase = PressedInside;
            if (onHighlightChange) onHighlightChange(true);
            return true;

        case PointerEvent::Move:
            // Drift-in capture (🎯T62): a Move arriving inside while idle
            // captures the touch — same effect as a Down inside. Lets the
            // user roll a finger onto a button from outside, matching
            // UIButton behaviour on iOS.
            if (phase == Idle) {
                if (!inside) return false;
                activeId = ev.id;
                phase = PressedInside;
                if (onHighlightChange) onHighlightChange(true);
                return true;
            }
            // Only the tracked finger drives state transitions; other
            // fingers' motion is ignored.
            if (ev.id != activeId) return false;
            if (inside && phase == PressedOutside) {
                phase = PressedInside;
                if (onHighlightChange) onHighlightChange(true);
            } else if (!inside && phase == PressedInside) {
                phase = PressedOutside;
                if (onHighlightChange) onHighlightChange(false);
            }
            return true;

        case PointerEvent::Up:
            if (phase == Idle) return false;
            if (ev.id != activeId) return false;
            {
                const bool fired = (phase == PressedInside);
                if (phase == PressedInside && onHighlightChange) {
                    onHighlightChange(false);
                }
                phase = Idle;
                activeId = 0;
                if (fired && onFire) onFire();
            }
            return true;
        }
        return false;
    }();

    if (onRedraw && phase != before) onRedraw();
    return consumed;
}

void Button::cancel() {
    if (phase == Idle) return;
    const bool wasHighlighted = (phase == PressedInside);
    phase = Idle;
    activeId = 0;
    if (wasHighlighted && onHighlightChange) onHighlightChange(false);
    if (onRedraw) onRedraw();  // 🎯T131.3 phase left Idle → draw the un-pressed frame
}

bool ButtonGroup::handleEvent(const PointerEvent& ev) {
    // 🎯T131.3 Fire the group's redraw sink on the routed button's visible-state
    // transition. handleEvent returns false only on no-op paths (no phase
    // change), so a change is possible exactly on the consumed path — snapshot
    // that one button's phase, no per-button scan.
    if (active) {
        Button*     b      = active;
        const auto  before = b->phase;
        b->handleEvent(ev);
        if (!active->tracking()) active = nullptr;
        if (onRedraw && b->phase != before) onRedraw();
        return true;  // while locked, the group claims every event
    }
    for (auto* b : buttons) {
        const auto before = b->phase;
        if (b->handleEvent(ev)) {
            if (b->tracking()) active = b;
            if (onRedraw && b->phase != before) onRedraw();
            return true;
        }
    }
    return false;
}

} // namespace ge
