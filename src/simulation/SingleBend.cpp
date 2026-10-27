#include "bending/simulation/SingleBend.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bending::simulation {
bool validSingleBend(const SingleBend &m) noexcept {
    const auto size = [](double value, double minimum) {
        // Range-boundary noise after STEP measurement, in millimetres.
        return std::isfinite(value) && value >= minimum - 1.0e-9 && value <= 1.0e5 + 1.0e-9;
    };
    return size(m.thicknessMm, 0.1) && size(m.innerRadiusMm, 0.1) && size(m.widthMm, 1.0) &&
           size(m.fixedFlangeMm, 1.0) && size(m.movingFlangeMm, 1.0) &&
           std::isfinite(m.angleRadians) &&
           m.angleRadians >= 5.0 * std::numbers::pi / 180.0 - 1.0e-12 &&
           m.angleRadians <= 175.0 * std::numbers::pi / 180.0 + 1.0e-12;
}
double bendAllowanceMm(const SingleBend &m) noexcept {
    return (m.innerRadiusMm + m.thicknessMm / 2.0) * m.angleRadians;
}
double blankLengthMm(const SingleBend &m) noexcept {
    return m.fixedFlangeMm + bendAllowanceMm(m) + m.movingFlangeMm;
}
std::optional<BendSection> singleBendSection(const SingleBend &m, double progress) noexcept {
    if (!validSingleBend(m) || !std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
        return {};
    }
    const double angle = progress * m.angleRadians;
    const double allowance = bendAllowanceMm(m);
    const auto arcPoint = [&](double fraction, double offset) -> Point2Mm {
        const double a = fraction * angle;
        // Avoid cancellation near the flat state. 1-cos(a) = 2*sin(a/2)^2.
        const double sinc = std::abs(a) < 1.0e-5 ? 1.0 - a * a / 6.0 : std::sin(a) / a;
        const double cosc = std::abs(a) < 1.0e-5 ? a / 2.0 - a * a * a / 24.0
                                                 : 2.0 * std::pow(std::sin(a / 2.0), 2) / a;
        return {fraction * allowance * sinc + offset * std::sin(a),
                fraction * allowance * cosc - m.thicknessMm / 2.0 - offset * std::cos(a)};
    };
    BendSection state{};
    state.fixed = {
        {{0, 0}, {-m.fixedFlangeMm, 0}, {-m.fixedFlangeMm, -m.thicknessMm}, {0, -m.thicknessMm}}};
    for (std::size_t i = 0; i < 3; ++i) {
        state.innerArc[i] = arcPoint(static_cast<double>(i) / 2.0, -m.thicknessMm / 2.0);
        state.outerArc[i] = arcPoint(static_cast<double>(i) / 2.0, m.thicknessMm / 2.0);
    }
    const auto inner = state.innerArc[2];
    const auto outer = state.outerArc[2];
    const double dx = m.movingFlangeMm * std::cos(angle);
    const double dy = m.movingFlangeMm * std::sin(angle);
    state.moving = {{inner, outer, {outer.x + dx, outer.y + dy}, {inner.x + dx, inner.y + dy}}};
    state.angleRadians = angle;
    if (angle > 0.0) {
        const double radius = allowance / angle - m.thicknessMm / 2.0;
        if (std::isfinite(radius)) {
            state.innerRadiusMm = radius;
        }
    }
    return state;
}
std::optional<Point2Mm> mapBendPoint(const SingleBend &m, Point2Mm p,
                                     const BendSection &target) noexcept {
    if (!validSingleBend(m) || !std::isfinite(p.x) || !std::isfinite(p.y) ||
        !std::isfinite(target.angleRadians) || target.angleRadians < 0 ||
        target.angleRadians > m.angleRadians + 1.0e-10) {
        return {};
    }
    const auto canonical =
        singleBendSection(m, std::clamp(target.angleRadians / m.angleRadians, 0.0, 1.0));
    if (!canonical) {
        return {};
    }
    // For 5..175 degrees the two flanges and sector occupy the polar interval
    // [-pi/2, angle+pi/2]. Unwrap before classifying obtuse moving flanges.
    double phi = std::atan2(p.x, m.innerRadiusMm - p.y);
    if (phi < -std::numbers::pi / 2) {
        phi += 2 * std::numbers::pi;
    }
    Point2Mm mapped{};
    if (phi <= 0) {
        mapped = p;
    } else if (phi >= m.angleRadians) {
        const auto end = singleBendSection(m, 1)->innerArc[2];
        const double c = std::cos(m.angleRadians), s = std::sin(m.angleRadians);
        const double along = (p.x - end.x) * c + (p.y - end.y) * s;
        const double depth = (p.x - end.x) * s - (p.y - end.y) * c;
        const double ct = std::cos(target.angleRadians), st = std::sin(target.angleRadians);
        mapped = {canonical->innerArc[2].x + along * ct + depth * st,
                  canonical->innerArc[2].y + along * st - depth * ct};
    } else {
        const double fraction = phi / m.angleRadians;
        const double a = fraction * target.angleRadians;
        const double sinc = std::abs(a) < 1.0e-5 ? 1 - a * a / 6 : std::sin(a) / a;
        const double cosc =
            std::abs(a) < 1.0e-5 ? a / 2 - a * a * a / 24 : 2 * std::pow(std::sin(a / 2), 2) / a;
        const double offset =
            std::hypot(p.x, m.innerRadiusMm - p.y) - m.innerRadiusMm - m.thicknessMm / 2;
        mapped = {fraction * bendAllowanceMm(m) * sinc + offset * std::sin(a),
                  fraction * bendAllowanceMm(m) * cosc - m.thicknessMm / 2 - offset * std::cos(a)};
    }
    // The contact model additionally rotates/translates the canonical section.
    const double dx = target.fixed[0].x - target.fixed[1].x;
    const double dy = target.fixed[0].y - target.fixed[1].y;
    const double length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length <= 0) {
        return {};
    }
    return Point2Mm{target.fixed[0].x + (mapped.x * dx - mapped.y * dy) / length,
                    target.fixed[0].y + (mapped.x * dy + mapped.y * dx) / length};
}
} // namespace bending::simulation
