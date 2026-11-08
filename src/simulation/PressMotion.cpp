#include "bending/simulation/PressMotion.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
PressMotionResult preparePressMotion(const SingleBend &m, core::PressTools tools, double tolerance,
                                     std::optional<DieShoulders> shoulders,
                                     std::optional<double> requiredAxialWidthMm) noexcept {
    const auto initial = flatToolSetup(m, tools, tolerance, shoulders, requiredAxialWidthMm);
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
    const double a = shoulders ? shoulders->halfSpanMm : tools.openingMm / 2.0;
    const double shoulderRadius = shoulders ? shoulders->radiusMm : 0.;
    const double sine = std::sin(half), cosine = std::cos(half);
    // Preserve the sufficient contact condition for the complete interval.
    // It also establishes a > halfLength, required by the endpoint proof below.
    const double maximumTangencyX = halfLength + m.thicknessMm / 2.0 * sine;
    if (a - shoulderRadius * sine - maximumTangencyX <= tolerance) {
        return PressMotionIssue::SupportNotProven;
    }
    // q(h)=(a-L*sinc(h)-b*sin(h))/cos(h), b=t/2+r. Under the condition above,
    // the numerator of q'(h) is increasing: q has no interior maximum.
    // See docs/bend-support-search-proposal.md for the continuous proof.
    const double b = m.thicknessMm / 2.0 + shoulderRadius;
    const double endQ = (a - halfLength * sinc(half) - b * sine) / cosine;
    const double roundoff =
        32 * std::numeric_limits<double>::epsilon() * (a + halfLength + b) / cosine;
    const double maximumQ = std::max(a - halfLength, endQ) + roundoff;
    if (!std::isfinite(maximumQ) ||
        maximumQ >= std::min(m.fixedFlangeMm, m.movingFlangeMm) - tolerance) {
        return PressMotionIssue::SupportNotProven;
    }
    return PressMotion{std::get<FlatToolSetup>(initial), maximumQ, tolerance, shoulders,
                       requiredAxialWidthMm};
}
Point2Mm dieSupportPoint(const PressMotion &motion, double angle) noexcept {
    const double a =
        motion.shoulders ? motion.shoulders->halfSpanMm : motion.initial.tools.openingMm / 2;
    const double r = motion.shoulders ? motion.shoulders->radiusMm : 0.;
    return {a - r * std::sin(angle / 2), r * (std::cos(angle / 2) - 1)};
}
static std::optional<PressState> calculateState(const SingleBend &m, const PressMotion &motion,
                                                double progress) noexcept {
    constexpr double approach = 0.2;
    const double forming = std::min(progress / pressBendingEndProgress, 1.0);
    const double fraction = forming <= approach ? 0.0 : (forming - approach) / (1.0 - approach);
    const double angle = fraction * m.angleRadians;
    const double h = angle / 2.0;
    const double c = std::cos(h), s = std::sin(h);
    const double halfLength = bendAllowanceMm(m) / 2.0;
    const double a =
        motion.shoulders ? motion.shoulders->halfSpanMm : motion.initial.tools.openingMm / 2.0;
    const double shoulderRadius = motion.shoulders ? motion.shoulders->radiusMm : 0.;
    const double oneMinusCos = 2.0 * std::pow(std::sin(h / 2.0), 2);
    const double outerX = halfLength * sinc(h) + m.thicknessMm / 2.0 * s;
    const double innerX = halfLength * sinc(h) - m.thicknessMm / 2.0 * s;
    const double outerRise = halfLength * cosc(h) + m.thicknessMm / 2.0 * oneMinusCos;
    const double innerRise = halfLength * cosc(h) - m.thicknessMm / 2.0 * oneMinusCos;
    const double bottom = (outerRise - a * s + shoulderRadius * oneMinusCos) / c;
    const double q = (a - shoulderRadius * s - outerX) / c;
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
    const double home = m.thicknessMm + motion.initial.tools.clearanceMm;
    const double gap =
        forming < approach ? motion.initial.tools.clearanceMm * (1.0 - forming / approach) : 0.0;
    const double formedTip = bottom + m.thicknessMm;
    const double returned =
        std::max(0.0, (progress - pressBendingEndProgress) / (1.0 - pressBendingEndProgress));
    const double tip = formedTip + gap + returned * (home - formedTip);
    const auto phase = progress > pressBendingEndProgress ? PressPhase::Retracting
                       : forming < approach               ? PressPhase::Approach
                                                          : PressPhase::Bending;
    return PressState{section, phase, tip, m.thicknessMm + motion.initial.tools.clearanceMm - tip,
                      q};
}
std::optional<PressState> pressMotionState(const SingleBend &m, const PressMotion &motion,
                                           double progress) noexcept {
    if (!std::isfinite(progress) || progress < 0 || progress > 1 ||
        !std::holds_alternative<PressMotion>(
            preparePressMotion(m, motion.initial.tools, motion.lengthToleranceMm, motion.shoulders,
                               motion.requiredAxialWidthMm)))
        return {};
    return calculateState(m, motion, progress);
}
std::optional<PressState> draftPressState(const SingleBend &m, core::PressTools tools,
                                          double progress,
                                          std::optional<DieShoulders> shoulders) noexcept {
    if (!validSingleBend(m) || !core::validPressTools(tools) || !std::isfinite(progress) ||
        progress < 0 || progress > 1)
        return {};
    PressMotion geometry{{tools, 0, 0, 0, 0, 0}, 0, 0, shoulders};
    return calculateState(m, geometry, (0.2 + 0.8 * progress) * pressBendingEndProgress);
}
} // namespace bending::simulation
