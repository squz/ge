// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Minimal appchannel stubs for the lean Android player link. The brokered
// player does not open an app-channel; Signal.cpp still references flush on
// the crash path, so provide a no-op rather than pulling the full channel +
// SDL_image + tweak graph from libge.

#include <ge/appchannel.h>

namespace ge::appchannel {

void flush(int) {}

}  // namespace ge::appchannel
