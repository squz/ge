// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Small layout helpers for fixed-row / fixed-column grids — menus,
// dialogs, panel rows. Pure math; no SDL or render dependency.
//
// Helpers return *positions* (the leading edge of row `i`); the
// caller decides whether to interpret as top-edge or center
// depending on its drawing convention.

#pragma once

namespace ge::layout {

// Y-coordinate of row `rowIdx` in a fixed-height row stack.
//
//   gridY(0, ..., topMargin) == topMargin
//   gridY(1, rowH, gap, ...) == topMargin + 1 * (rowH + gap)
//   gridY(N, rowH, gap, ...) == topMargin + N * (rowH + gap)
//
// `rowIdx` may be negative (allows positioning above the first row,
// e.g. for a header band).
constexpr float gridY(int rowIdx, float rowH, float gap, float topMargin) {
    return topMargin + static_cast<float>(rowIdx) * (rowH + gap);
}

// X-coordinate of column `colIdx` in a fixed-width column stack.
// Mirror of `gridY` (axis-swapped).
constexpr float gridX(int colIdx, float colW, float gap, float leftMargin) {
    return leftMargin + static_cast<float>(colIdx) * (colW + gap);
}

} // namespace ge::layout
