// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Web / other stub: physical RAM isn't queryable (wasm sandbox has no
// sysctl/sysinfo equivalent worth trusting), so report 0. Combined with
// tierFor, a 0 report still lands on Capped rather than Full whenever ASTC
// is sampleable — the conservative choice for an unknown-RAM device — and
// on Legacy when it isn't.

#include <cstdint>

namespace ge {

uint64_t platformPhysRamBytes() { return 0; }

} // namespace ge
