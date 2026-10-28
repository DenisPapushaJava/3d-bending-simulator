#pragma once
#include "bending/core/ToolGeometry.h"
#include <cmath>
#include <numbers>
namespace tool_fixture {
inline bending::core::ToolAssignment pair(double opening = 12., double radius = .8,
                                          double overhang = 0.) {
    using namespace bending::core;
    constexpr double pi = std::numbers::pi;
    ToolAssignment a;
    a.punch = {"own-punch", "Own fixture",
               "P60",       ToolKind::Punch,
               pi / 3,      80,
               45,          .8,
               {},          "Own synthetic geometry",
               "",          false};
    a.die = {"own-die", "Own fixture",
             "V60",     ToolKind::Die,
             pi / 3,    80,
             25,        {},
             {opening}, "Own synthetic geometry",
             "",        false};
    ToolDrawing p{"own-punch.dxf", std::string(64, 'a'), true, {}},
        d{"own-die.dxf", std::string(64, 'b'), true, {}};
    const auto arc = [](DrawingPoint c, double r, double start, double sweep) {
        return DrawingEdge{{c.x + r * std::cos(start), c.y + r * std::sin(start)},
                           {c.x + r * std::cos(start + sweep), c.y + r * std::sin(start + sweep)},
                           c,
                           sweep};
    };
    const auto line = [](DrawingPoint x, DrawingPoint y) { return DrawingEdge{x, y, {}, 0}; };
    const auto nose = arc({0, .8}, .8, 7 * pi / 6, 2 * pi / 3);
    const double w = nose.end.x + (10 - nose.end.y) * std::tan(pi / 6);
    p.edges = {nose,
               line(nose.end, {w, 10}),
               line({w, 10}, {w + overhang, 45}),
               line({w + overhang, 45}, {-w, 45}),
               line({-w, 45}, {-w, 10}),
               line({-w, 10}, nose.start)};
    const double c = opening / 2 + radius * std::tan(pi / 6);
    const auto left = arc({-c, -radius}, radius, pi / 2, -pi / 3);
    const auto right = arc({c, -radius}, radius, 5 * pi / 6, -pi / 3);
    const double bottom = left.end.y + left.end.x / std::tan(pi / 6);
    d.edges = {line({-20, 0}, left.start),
               left,
               line(left.end, {0, bottom}),
               line({0, bottom}, right.start),
               right,
               line(right.end, {20, 0}),
               line({20, 0}, {20, -25}),
               line({20, -25}, {-20, -25}),
               line({-20, -25}, {-20, 0})};
    a.punch.drawing = std::move(p);
    a.die.drawing = std::move(d);
    a.clearanceMm = 3;
    a.useDrawingGeometry = true;
    return a;
}
inline bending::core::ToolAssignment hook() {
    auto a = pair();
    auto &e = a.punch.drawing->edges;
    const auto nose = e.front();
    const double w = e[1].end.x;
    const auto line = [](auto x, auto y) { return bending::core::DrawingEdge{x, y, {}, 0}; };
    using P = bending::core::DrawingPoint;
    e = {nose,
         line(nose.end, P{w, 10}),
         line(P{w, 10}, P{w, 18}),
         line(P{w, 18}, P{2, 18}),
         line(P{2, 18}, P{2, 36}),
         line(P{2, 36}, P{w + 4, 36}),
         line(P{w + 4, 36}, P{w + 4, 45}),
         line(P{w + 4, 45}, P{-w, 45}),
         line(P{-w, 45}, P{-w, 10}),
         line(P{-w, 10}, nose.start)};
    return a;
}
} // namespace tool_fixture
namespace tool_fixture {
inline bending::core::ToolPreset fourWayDie() {
    using namespace bending::core;
    constexpr double pi = std::numbers::pi;
    auto t = pair().die;
    t.id = "own-four-way";
    t.code = "Own four-way";
    t.heightMm = 70;
    t.openingsMm = {12, 18, 24, 30};
    t.angleRadians = pi / 3;
    ToolDrawing drawing{"own-four-way.dxf", std::string(64, 'c'), true, {}};
    for (std::size_t i = 0; i < 4; ++i) {
        const double beta = (60 + 10 * static_cast<double>(i)) * pi / 180;
        const double tilt = (pi - beta) / 2, r = .6 + .3 * static_cast<double>(i),
                     c = t.openingsMm[i] / 2 + r * std::tan(tilt / 2);
        const auto arc = [&](DrawingPoint centre, double angle, double sweep) {
            const auto at = [&](double a) {
                return DrawingPoint{centre.x + r * std::cos(a), centre.y + r * std::sin(a)};
            };
            return DrawingEdge{at(angle), at(angle + sweep), centre, sweep};
        };
        const auto l = arc({-c, 35 - r}, pi / 2, -tilt),
                   right = arc({c, 35 - r}, pi / 2 + tilt, -tilt);
        const DrawingPoint bottom{0, l.end.y + l.end.x * std::tan(tilt)};
        ToolDrawing side{"own.dxf",
                         std::string(64, 'd'),
                         true,
                         {{{-35, 35}, l.start, {}, 0},
                          l,
                          {l.end, bottom, {}, 0},
                          {bottom, right.start, {}, 0},
                          right,
                          {right.end, {35, 35}, {}, 0}}};
        const double turn = i == 3 ? pi / 2 : -static_cast<double>(i) * pi / 2;
        const auto placed = rotateToolDrawing(side, turn);
        drawing.edges.insert(drawing.edges.end(), placed.edges.begin(), placed.edges.end());
    }
    t.drawing = std::move(drawing);
    for (const auto &g : detectDieGrooves(*t.drawing))
        t.grooveSettings.push_back(g.settings);
    return t;
}
} // namespace tool_fixture
