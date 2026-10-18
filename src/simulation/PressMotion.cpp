#include "bending/simulation/PressMotion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bending::simulation {
namespace {
double sinc(double a) noexcept {
    return std::abs(a) < 1.0e-5 ? 1.0 - a * a / 6.0 : std::sin(a) / a;
}
double cosc(double a) noexcept {
    return std::abs(a) < 1.0e-5 ? a / 2.0 - a * a * a / 24.0
                                : 2.0 * std::pow(std::sin(a / 2.0), 2) / a;
}
} // namespace
PressMotionResult preparePressMotion(const SingleBend &m, core::PressTools tools,
                                     double tolerance) noexcept {
    const auto initial = flatToolSetup(m, tools, tolerance);
    if (!std::holds_alternative<FlatToolSetup>(initial)) {
        return PressMotionIssue::InvalidParameters;
    }
    // No overbend beyond the tool side angles. Equality permits wall contact at the endpoint.
    if (m.angleRadians + std::max(tools.dieAngleRadians, tools.punchAngleRadians) >
        std::numbers::pi + 1e-12) {
        return PressMotionIssue::ToolAngleConflict;
    }
    if (tools.noseRadiusMm > m.innerRadiusMm - tolerance) {
        return PressMotionIssue::NoseTooLarge;
    }
    const double half = m.angleRadians / 2.0;
    const double halfLength = bendAllowanceMm(m) / 2.0;
    const double a = tools.openingMm / 2.0;
    // For h in [0,half], sinc(h) decreases, sin(h) increases, cos(h) decreases.
    // Bound outer tangency X and q=(a-X)/cos(h) without a discrete-only support test.
    const double maximumTangencyX = halfLength + m.thicknessMm / 2.0 * std::sin(half);
    const double maximumQ = (a - halfLength * sinc(half)) / std::cos(half);
    if (a - maximumTangencyX <= tolerance ||
        maximumQ >= std::min(m.fixedFlangeMm, m.movingFlangeMm) - tolerance) {
        return PressMotionIssue::SupportNotProven;
    }
    return PressMotion{std::get<FlatToolSetup>(initial), maximumQ, tolerance};
}
std::optional<PressState> pressMotionState(const SingleBend &m, const PressMotion &motion,
                                           double progress) noexcept {
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0 ||
        !std::holds_alternative<PressMotion>(
            preparePressMotion(m, motion.initial.tools, motion.lengthToleranceMm))) {
        return {};
    }
    constexpr double approach = 0.2;
    const double fraction = progress <= approach ? 0.0 : (progress - approach) / (1.0 - approach);
    const double angle = fraction * m.angleRadians;
    const double h = angle / 2.0;
    const double c = std::cos(h), s = std::sin(h);
    const double halfLength = bendAllowanceMm(m) / 2.0;
    const double a = motion.initial.tools.openingMm / 2.0;
    const double oneMinusCos = 2.0 * std::pow(std::sin(h / 2.0), 2);
    const double outerX = halfLength * sinc(h) + m.thicknessMm / 2.0 * s;
    const double innerX = halfLength * sinc(h) - m.thicknessMm / 2.0 * s;
    const double outerRise = halfLength * cosc(h) + m.thicknessMm / 2.0 * oneMinusCos;
    const double innerRise = halfLength * cosc(h) - m.thicknessMm / 2.0 * oneMinusCos;
    const double bottom = (outerRise - a * s) / c;
    const double q = (a - outerX) / c;
    BendSection section{};
    section.innerArc = {{{-innerX, bottom + m.thicknessMm + innerRise},
                         {0, bottom + m.thicknessMm},
                         {innerX, bottom + m.thicknessMm + innerRise}}};
    section.outerArc = {{{-outerX, bottom + outerRise}, {0, bottom}, {outerX, bottom + outerRise}}};
    const auto il = section.innerArc[0], ol = section.outerArc[0];
    const auto ir = section.innerArc[2], oright = section.outerArc[2];
    section.fixed = {{il,
                      {il.x - m.fixedFlangeMm * c, il.y + m.fixedFlangeMm * s},
                      {ol.x - m.fixedFlangeMm * c, ol.y + m.fixedFlangeMm * s},
                      ol}};
    section.moving = {{ir,
                       oright,
                       {oright.x + m.movingFlangeMm * c, oright.y + m.movingFlangeMm * s},
                       {ir.x + m.movingFlangeMm * c, ir.y + m.movingFlangeMm * s}}};
    section.angleRadians = angle;
    if (angle > 0.0) {
        const double radius = bendAllowanceMm(m) / angle - m.thicknessMm / 2.0;
        if (std::isfinite(radius)) {
            section.innerRadiusMm = radius;
        }
    }
    const double gap =
        progress < approach ? motion.initial.tools.clearanceMm * (1.0 - progress / approach) : 0.0;
    const double tip = bottom + m.thicknessMm + gap;
    return PressState{section, progress < approach ? PressPhase::Approach : PressPhase::Bending,
                      tip, m.thicknessMm + motion.initial.tools.clearanceMm - tip, q};
}
} // namespace bending::simulation
