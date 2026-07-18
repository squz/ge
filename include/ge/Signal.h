// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Signal handling for macOS CLI binaries (server and player).
// No-op on iOS/Android where the OS manages app lifecycle.
#pragma once

#include <ge/Linalg.h>

#include <exception>
#include <utility>

namespace ge {

// Install SIGINT/SIGTERM handlers. Call once at startup.
void installSignalHandlers();

// True after SIGINT or SIGTERM received.
bool shouldQuit();

// ── 🎯T136 Crash diagnostics ──────────────────────────────────────
//
// When a consumer's callback crashes inside a ge run loop, ge emits a last-gasp
// record through the SAME app-channel logger spyder drains (app_log_get / log
// capture) before the process dies — so the failure is visible without pulling
// and hand-symbolicating device IPS crash files. Two halves: uncaught C++
// exceptions out of onUpdate/onRender/onEvent (guardCallback), and fatal
// signals — SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE (installCrashHandlers).

// Install last-gasp handlers for the fatal crash signals. Each writes the
// signal + a best-effort backtrace to stderr and forwards it through the
// app-channel logger, then restores the default handler and re-raises so the
// OS still writes its own crash report. Distinct from installSignalHandlers()
// (graceful SIGINT/SIGTERM quit) and — unlike it — active on iOS/Android too,
// where the developer otherwise has only an IPS file. Call once at startup.
void installCrashHandlers();

// Whether crash diagnostics are enabled (the guardCallback wrap + the crash
// signal handlers). Defaults to true; the direct run loop sets it from
// SessionHostConfig::crashDiagnostics so a tool/test that wants raw aborts can
// opt out.
bool crashDiagnosticsEnabled();
void setCrashDiagnosticsEnabled(bool on);

// Log an uncaught exception thrown out of a named consumer callback through the
// app-channel logger (flushed), before it propagates on to terminate (→ the
// SIGABRT handler). Implementation detail of guardCallback.
void reportCallbackException(const char* callback, const char* what);

// Run a consumer callback, surfacing an uncaught exception through the
// app-channel logger before re-throwing (so the OS crash report is unchanged).
// When crash diagnostics are disabled it calls fn() raw — a tool that expects
// exceptions to propagate untouched is unaffected.
//
// In a TU compiled without exceptions (🎯T157: SessionHost.mm on web is
// -fno-exceptions so the Asyncify suspend chain contains no JS invoke
// trampolines) the guard degrades to a raw call — an uncaught exception
// aborts, which is web's crash behaviour anyway.
template <class F>
inline void guardCallback(const char* name, F&& fn) {
#if defined(__cpp_exceptions)
    if (!crashDiagnosticsEnabled()) {
        std::forward<F>(fn)();
        return;
    }
    try {
        std::forward<F>(fn)();
    } catch (const std::exception& e) {
        reportCallbackException(name, e.what());
        throw;
    } catch (...) {
        reportCallbackException(name, "unknown (non-std::exception)");
        throw;
    }
#else
    (void)name;
    std::forward<F>(fn)();
#endif
}

} // namespace ge
