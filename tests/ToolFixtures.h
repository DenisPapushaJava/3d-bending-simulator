#pragma once
#include "bending/core/ToolGeometry.h"
#include <algorithm>
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
inline bending::core::ToolPreset offsetPunch(bool mirrored = false) {
    auto punch = pair().punch;
    punch.id = mirrored ? "offset-mirrored" : "offset";
    punch.code = mirrored ? "Offset mirrored" : "Offset";
    auto &e = punch.drawing->edges;
    e[2].end.x = -30;
    e[3].start.x = -30;
    e[3].end.x = -50;
    e[4].start.x = -50;
    if (mirrored)
        for (auto &edge : e) {
            edge.start.x = -edge.start.x;
            edge.end.x = -edge.end.x;
            if (edge.center)
                edge.center->x = -edge.center->x;
            edge.sweepRadians = -edge.sweepRadians;
        }
    return punch;
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
inline bending::core::ToolAssignment tangPair() {
    using namespace bending::core;
    auto a = pair(16, 1, 30);
    auto &edges = a.punch.drawing->edges;
    const auto top = edges[3];
    const auto line = [](DrawingPoint p, DrawingPoint q) { return DrawingEdge{p, q, {}, 0}; };
    edges.erase(edges.begin() + 3);
    const std::vector<DrawingEdge> neck{line(top.start, {7, 45}), line({7, 45}, {7, 75}),
                                        line({7, 75}, {-5, 75}), line({-5, 75}, {-5, 45}),
                                        line({-5, 45}, top.end)};
    edges.insert(edges.begin() + 3, neck.begin(), neck.end());
    a.punch.heightMm = 75;
    a.machineModel = MachineModel::DemoPressV2;
    return a;
}
} // namespace tool_fixture
namespace tool_fixture {
inline bending::core::ToolPreset fourWayDie(std::vector<double> openings = {12, 18, 24, 30}) {
    using namespace bending::core;
    constexpr double pi = std::numbers::pi;
    auto t = pair().die;
    t.id = "own-four-way";
    t.code = "Own four-way";
    t.heightMm = 70;
    t.openingsMm = std::move(openings);
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

namespace tool_fixture {
// Owned, explicit finite contours for GUI regressions. Never part of the application catalogue.
inline bending::core::ToolAssignment explicitPair(const bending::core::PressTools &tools) {
    using namespace bending::core;
    constexpr double pi = std::numbers::pi;
    ToolAssignment a;
    const auto key = std::to_string(tools.openingMm) + "-" + std::to_string(tools.dieAngleRadians) +
                     "-" + std::to_string(tools.punchAngleRadians) + "-" +
                     std::to_string(tools.noseRadiusMm) + "-" + std::to_string(tools.lengthMm) +
                     "-" + std::to_string(punchLengthMm(tools));
    a.punch = {"fixture-p-" + key,
               "Own GUI fixture",
               "Owned punch",
               ToolKind::Punch,
               tools.punchAngleRadians,
               punchLengthMm(tools),
               tools.openingMm,
               tools.noseRadiusMm,
               {},
               "Own explicit test contour",
               "",
               false};
    a.die = {"fixture-d-" + key,
             "Own GUI fixture",
             "Owned die",
             ToolKind::Die,
             tools.dieAngleRadians,
             tools.lengthMm,
             {},
             {},
             {tools.openingMm},
             "Own explicit test contour",
             "",
             false};
    ToolDrawing punch{"owned-punch.dxf", std::string(64, 'a'), true, {}},
        die{"owned-die.dxf", std::string(64, 'b'), true, {}};
    const auto line = [](DrawingPoint x, DrawingPoint y) { return DrawingEdge{x, y, {}, 0}; };
    const auto arc = [](DrawingPoint c, double r, double start, double sweep) {
        return DrawingEdge{{c.x + r * std::cos(start), c.y + r * std::sin(start)},
                           {c.x + r * std::cos(start + sweep), c.y + r * std::sin(start + sweep)},
                           c,
                           sweep};
    };
    const auto half = tools.punchAngleRadians / 2;
    const auto nose =
        arc({0, tools.noseRadiusMm}, tools.noseRadiusMm, pi + half, pi - tools.punchAngleRadians);
    const double x = nose.end.x + (tools.openingMm - nose.end.y) * std::tan(half);
    punch.edges = {line({-x, tools.openingMm}, nose.start), nose,
                   line(nose.end, {x, tools.openingMm}),
                   line({x, tools.openingMm}, {-x, tools.openingMm})};
    const double tilt = (pi - tools.dieAngleRadians) / 2, r = .2,
                 c = tools.openingMm / 2 + r * std::tan(tilt / 2);
    const auto left = arc({-c, -r}, r, pi / 2, -tilt),
               right = arc({c, -r}, r, pi / 2 + tilt, -tilt);
    const double bottom = left.end.y + left.end.x * std::tan(tilt),
                 outer = std::max(tools.openingMm, c + 10), base = bottom - tools.openingMm / 2;
    die.edges = {line({-outer, 0}, left.start),
                 left,
                 line(left.end, {0, bottom}),
                 line({0, bottom}, right.start),
                 right,
                 line(right.end, {outer, 0}),
                 line({outer, 0}, {outer, base}),
                 line({outer, base}, {-outer, base}),
                 line({-outer, base}, {-outer, 0})};
    a.die.heightMm = -base;
    a.punch.drawing = std::move(punch);
    a.die.drawing = std::move(die);
    a.clearanceMm = tools.clearanceMm;
    a.machineModel = tools.machineModel;
    a.punchReversed = tools.punchReversed;
    a.useDrawingGeometry = true;
    return a;
}
// Owned contours for the guided UI regression, independent of product catalogues.
inline bending::core::ToolCatalog guidedCatalog() {
    using namespace bending::core;
    const double pi = std::numbers::pi;
    auto wide = explicitPair({48, pi / 3, 26 * pi / 180, .8, 835, 10});
    wide.punch.id = "fixture-punch-long";
    wide.punch.code = "ST1801M";
    wide.punch.lengthMm = 525;
    wide.die = fourWayDie({10, 16, 22, 35});
    wide.die.id = "fixture-four-way";
    wide.die.code = "ST2401S";
    wide.die.lengthMm = 835;
    auto narrow = explicitPair({12, pi / 3, pi / 3, .8, 835, 10});
    narrow.punch.id = "fixture-punch";
    narrow.punch.code = "ST1405S";
    narrow.die.id = "fixture-die";
    narrow.die.code = "ST3412S";
    return {wide.die, wide.punch, narrow.die, narrow.punch};
}
} // namespace tool_fixture
