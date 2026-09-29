#pragma once

#include <algorithm>
#include <cmath>

namespace gx {
struct WindowArea { int x, y, w, h; };
struct WindowBorders { int top, left, bottom, right; };

// Desktop coordinates are SDL window units, not high-DPI framebuffer pixels.
// Leave room around the decorated window so its title bar and edges are reachable.
inline WindowArea initialWindowLayout(WindowArea desktop, double aspect, int windowScale = 0,
                                      WindowBorders borders = {48, 8, 8, 8}) {
    if (!std::isfinite(aspect) || aspect <= 0) aspect = 4.0 / 3.0;
    const int maxW = std::max(1, int(desktop.w * 0.8) - borders.left - borders.right);
    const int maxH = std::max(1, int(desktop.h * 0.8) - borders.top - borders.bottom);
    double height = windowScale > 0 ? 480.0 * windowScale : 720.0;
    double width = height * aspect;
    // A predictable default, independent of the internal rendering resolution.
    if (windowScale <= 0 && width > 1280) { height *= 1280 / width; width = 1280; }
    const double fit = std::min({1.0, maxW / width, maxH / height});
    const int w = std::max(1, int(std::round(width * fit)));
    const int h = std::max(1, int(std::round(height * fit)));
    return {desktop.x + (desktop.w - w - borders.left - borders.right) / 2 + borders.left,
            desktop.y + (desktop.h - h - borders.top - borders.bottom) / 2 + borders.top, w, h};
}
}  // namespace gx
