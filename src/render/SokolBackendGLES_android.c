// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// libgesokol-gles.so (🎯T107) — the GLES3 sokol_gfx backend behind ONE exported
// entrypoint. All 150 sg_* (and sokol's GLES impl) are compiled here with
// hidden visibility; only ge_sokol_bind_gles() is exported, named per backend
// so the loader picks exactly one and co-mapping can never collide. The bind
// installs this .so's sg_* into the dispatch table that libge's forwarder shim
// consults. ge_sokol_set_api lives in libge's shim (resolved at the consumer's
// libmain.so link). sokol calls GL against whatever context SokolContext made
// current via SDL/EGL, so this .so links GLESv3/EGL but owns no window/context.

// sokol_gfx.h's IMPL typedefs are unguarded, so it must be #included exactly
// once with SOKOL_IMPL. The dispatch header pulls the (public, backend-agnostic)
// API first; we then pull the IMPL once. SOKOL_GLES3 is set before both so the
// single backend choice is consistent.
#define SOKOL_GLES3
#include "ge_sokol_dispatch.h"  // ge_sg_api, ge_sokol_set_api + sokol_gfx.h API

#define SOKOL_IMPL
#include "sokol_gfx.h"          // IMPL only (public section already guarded)

__attribute__((visibility("default")))
void ge_sokol_bind_gles(void) {
    static ge_sg_api a;
    ge_sg_api* _a = &a;
#include "ge_sokol_fill.inc"     // _a->sg_X = sg_X;  (this .so's hidden impl)
    ge_sokol_set_api(&a);
}
