#pragma once
#include "bending/core/ToolCatalog.h"
#include <QPainterPath>
#include <cmath>
#include <numbers>
namespace bending::ui {
inline QPainterPath toolDrawingPath(const core::ToolDrawing &drawing) {
    QPainterPath path;
    if (drawing.edges.empty())
        return path;
    path.moveTo(drawing.edges.front().start.x, -drawing.edges.front().start.y);
    for (const auto &edge : drawing.edges) {
        if (!edge.center) {
            path.lineTo(edge.end.x, -edge.end.y);
            continue;
        }
        const double r = std::hypot(edge.start.x - edge.center->x, edge.start.y - edge.center->y);
        const double start =
            std::atan2(edge.start.y - edge.center->y, edge.start.x - edge.center->x);
        path.arcTo(edge.center->x - r, -edge.center->y - r, 2 * r, 2 * r,
                   start * 180 / std::numbers::pi, edge.sweepRadians * 180 / std::numbers::pi);
    }
    path.closeSubpath();
    return path;
}
} // namespace bending::ui
