// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Re-exports linalg's full alias set into `ge::la`. Every public ge
// header includes this so games can write `ge::la::float2` /
// `ge::la::float4x4` / `ge::la::int3` no matter which ge header they
// pulled in. `using namespace ge::la;` brings the short forms in
// unqualified for game code that wants them.
//
// Sub-namespace `la` (not directly into `ge::`) is deliberate —
// keeps linalg's 96 aliases out of `ge::`'s autocomplete and
// preserves a clean visual marker at use sites that the type came
// from linalg, not ge proper.
#pragma once

#include <linalg.h>

namespace ge::la {
using namespace linalg::aliases;

// Re-export common matrix/vector ops so `ge::la::mul`, `ge::la::inverse`,
// etc. resolve without a separate `using linalg::...` at the call site.
// linalg's free functions live in the `linalg` namespace itself (not
// `aliases`), so they're not picked up by the `using namespace` above.
using linalg::mul;
using linalg::inverse;
using linalg::transpose;
using linalg::dot;
using linalg::cross;
using linalg::length;
using linalg::length2;
using linalg::normalize;
using linalg::distance;
using linalg::lerp;
using linalg::clamp;
using linalg::minelem;
using linalg::maxelem;
}

// A semantic alias for a straight-alpha RGBA colour (components in [0, 1]).
// Promoted to `ge::` (not `ge::la`) because it's a domain concept, not a raw
// linalg alias — used across the rendering surface (ge::debug, text, …).
namespace ge { using Color = la::float4; }
