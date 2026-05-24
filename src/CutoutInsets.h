// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Engine-internal: query the platform for display-cutout insets only
// (camera notch / Dynamic Island / status-bar-when-visible — i.e. the
// regions where pixels are physically obscured by hardware or system UI
// drawn on top). Distinct from the full input-safe insets which also
// include OS-reserved gesture / tappable zones.
//
// On iOS, the platform doesn't expose a clean cutout-only signal, so
// the value matches the full SDL safe-area (best the platform allows;
// drawSafeRectInPts == uiSafeRectInPts on iOS as a result).
//
// On Android, queried via JNI from the activity's
// `getDisplayCutoutInsets()` helper — `WindowInsets.Type.displayCutout()`.
#pragma once

#include <ge/SessionHost.h>

namespace ge {

// Returns insets in pixel units (Android JNI uses pixels). Callers
// are responsible for dividing by pixelsPerPt to convert to pt before
// passing to Context::setDrawSafeInsets. Zero on desktop and on
// platforms where the cutout query isn't available.
SafeAreaInsets queryDisplayCutoutInsets();

} // namespace ge
