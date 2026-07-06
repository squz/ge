// 🎯T140/T141 regression oracle for the sokol dispatch shim.
//
// The shim's teardown forwarders (sg_isvalid / sg_shutdown) must tolerate an
// UNBOUND table: g_ge_sg_api is NULL until ge_sokol_set_api() runs, and on the
// Android VK->GLES fallback a partially-constructed VkM/GlesM destructor calls
// these forwarders while the table is still NULL. Before the fix (fable-2026-07
// F2/F3/F4) each forwarder dereferenced g_ge_sg_api unconditionally, so the
// process SIGSEGV'd on the very fallback path that should degrade to GLES.
//
// Standalone by necessity: the generated shim defines real sg_* symbols, which
// collide with sokol's SOKOL_IMPL in the doctest binary (libge on Apple). Build
// and run directly — `make dispatch-null-safety-test` from the ge repo root.
//
// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "ge_sokol_dispatch.h"

static int g_shutdown_called = 0;
static bool stub_isvalid(void) { return true; }
static void stub_shutdown(void) { g_shutdown_called = 1; }

int main(void) {
    // 1) UNBOUND table — the VkM/GlesM teardown-after-failed-init state.
    g_ge_sg_api = 0;
    if (sg_isvalid()) { printf("FAIL: sg_isvalid()==true on unbound table\n"); return 1; }
    sg_shutdown();  // must be a no-op, must NOT deref NULL
    if (g_shutdown_called) { printf("FAIL: sg_shutdown forwarded on unbound table\n"); return 1; }

    // 2) BOUND table — happy path unchanged (forwards to the backend).
    static const ge_sg_api tbl = { .sg_isvalid = stub_isvalid, .sg_shutdown = stub_shutdown };
    ge_sokol_set_api(&tbl);
    if (!sg_isvalid()) { printf("FAIL: sg_isvalid()==false on bound table\n"); return 1; }
    sg_shutdown();
    if (!g_shutdown_called) { printf("FAIL: sg_shutdown not forwarded on bound table\n"); return 1; }

    printf("PASS: dispatch shim teardown forwarders are NULL-safe\n");
    return 0;
}
