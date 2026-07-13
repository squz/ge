// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ServerSession — multi-session server side of the brokered wire (restored
// per-player independence from the Dawn/bgfx multi-session design, adapted
// for sokol + spyder).
//
// One sideband to the relay (/ws/server?name=). Each player_attached opens an
// independent /ws/server/wire/<id> with its own encoder and input queue. There
// is no broadcast: each session is a fully independent game (factory + render +
// encode + wire). The game loop (runServer) owns RunConfig state per session;
// this class owns networking + encode only.
//
// Capture routing: setCaptureTarget(sessionId) then render; onCapturedFrame
// encodes only to that session's wire.
#pragma once

#include <ge/Protocol.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>

namespace ge {

class ServerSession {
public:
    ServerSession(std::string host, int port, std::string name,
                  const wire::SessionConfig& cfg);
    ~ServerSession();

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    void start();
    void stop();

    // True while at least one player session is attached.
    bool active() const;
    std::atomic<bool>* activeFlag();

    // Game-thread lifecycle: poll attach/detach queues filled by the sideband.
    // onAttach is called once per new session (wire already open). onDetach
    // after the wire is closed — drop game state for that id.
    void pollLifecycle(const std::function<void(const std::string& id)>& onAttach,
                       const std::function<void(const std::string& id)>& onDetach);

    // Active session ids (stable for the current frame after pollLifecycle).
    std::vector<std::string> sessionIds() const;

    // Drain queued SDL events for one session (from its wire). Call on the
    // game thread; do not share input across sessions.
    void drainInput(const std::string& sessionId,
                    const std::function<void(const SDL_Event&)>& deliver);

    // Which session receives the next onCapturedFrame encode.
    void setCaptureTarget(const std::string& sessionId);

    // Game thread: encode + send a just-captured frame to the capture target.
    void onCapturedFrame(const std::uint8_t* px, int w, int h);

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace ge
