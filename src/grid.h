#pragma once

#include <windows.h>

#include <cmath>

#include "settings.h"

namespace sc {

// One axis of a grid: where the lines fall along the width or height of a
// rectangle.
//
// A pixel pitch and a division count are the same idea measured differently, so
// both reduce to "which line is number i, and which number is nearest to this
// coordinate". Division uses round(i * size / n) rather than i * (size / n),
// because the accumulated error of the latter leaves the last line short of the
// edge, which is the one thing division exists to get right.
//
// Lines continue past the rectangle in both directions at the same spacing, so
// a selection dragged onto the next screen keeps snapping.
//
// Shared by the capture selection and window snapping. The two measure
// different rectangles - the capture grid covers the monitor, the window grid
// its work area - but the arithmetic is the same.
class GridAxis {
public:
    GridAxis(LONG start, LONG size, int px, int divisions)
        : start_(start),
          size_(size > 0 ? size : 1),
          px_(px),
          divisions_(divisions > 0 ? divisions : 1) {}

    LONG Line(long long index) const {
        if (px_ > 0) {
            return start_ + static_cast<LONG>(index * px_);
        }
        return start_ +
               static_cast<LONG>(std::llround(static_cast<double>(index) * size_ / divisions_));
    }

    long long NearestIndex(LONG value) const { return std::llround(Position(value)); }

    long long IndexBelow(LONG value) const {
        return static_cast<long long>(std::floor(Position(value)));
    }

    LONG Snap(LONG value) const { return Line(NearestIndex(value)); }

private:
    // The coordinate expressed in grid lines, which need not be a whole number.
    double Position(LONG value) const {
        const double offset = static_cast<double>(value - start_);
        return px_ > 0 ? offset / px_ : offset * divisions_ / size_;
    }

    LONG start_;
    LONG size_;
    int px_;
    int divisions_;
};

inline GridAxis HorizontalAxis(const RECT& area, const settings::GridChoice& grid) {
    return GridAxis(area.left, area.right - area.left, grid.px, grid.cols);
}

inline GridAxis VerticalAxis(const RECT& area, const settings::GridChoice& grid) {
    return GridAxis(area.top, area.bottom - area.top, grid.px, grid.rows);
}

}  // namespace sc
