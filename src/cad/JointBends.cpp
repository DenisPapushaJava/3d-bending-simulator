#include "JointBends.h"
#include <algorithm>
#include <cmath>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <set>

namespace bending::cad {
std::optional<application::RecognizedBend>
jointBendEnvelope(const application::BendAnalysis &analysis, std::span<const std::size_t> indices) {
    if (indices.size() < 2 || indices.size() > 32 ||
        analysis.status != application::BendRecognitionStatus::Completed ||
        indices.front() >= analysis.bends.size() || !std::isfinite(analysis.lengthToleranceMm) ||
        analysis.lengthToleranceMm <= 0 || !std::isfinite(analysis.angleToleranceRadians) ||
        analysis.angleToleranceRadians <= 0)
        return {};
    const auto point = [](application::PointMm p) { return gp_Pnt(p.x, p.y, p.z); };
    auto result = analysis.bends[indices.front()];
    const auto finite = [](application::PointMm p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    };
    if (!finite(result.axisStart) || !finite(result.axisEnd) ||
        point(result.axisStart).Distance(point(result.axisEnd)) <= analysis.lengthToleranceMm)
        return {};
    const auto origin = point(result.axisStart);
    const gp_Dir direction(gp_Vec(origin, point(result.axisEnd)));
    const gp_Lin axis(origin, direction);
    const double tolerance = analysis.lengthToleranceMm;
    std::vector<std::pair<double, double>> intervals;
    std::set<std::size_t> unique;
    for (const auto index : indices) {
        if (index >= analysis.bends.size() || !unique.insert(index).second)
            return {};
        const auto &bend = analysis.bends[index];
        if (!finite(bend.axisStart) || !finite(bend.axisEnd) || !std::isfinite(bend.thicknessMm) ||
            !std::isfinite(bend.innerRadiusMm) || !std::isfinite(bend.angleRadians) ||
            bend.thicknessMm <= 0 || bend.innerRadiusMm <= 0 || bend.angleRadians <= 0 ||
            std::abs(bend.thicknessMm - result.thicknessMm) > tolerance ||
            std::abs(bend.innerRadiusMm - result.innerRadiusMm) > tolerance ||
            std::abs(bend.angleRadians - result.angleRadians) > analysis.angleToleranceRadians ||
            axis.Distance(point(bend.axisStart)) > tolerance ||
            axis.Distance(point(bend.axisEnd)) > tolerance)
            return {};
        const double a = gp_Vec(origin, point(bend.axisStart)).Dot(gp_Vec(direction));
        const double b = gp_Vec(origin, point(bend.axisEnd)).Dot(gp_Vec(direction));
        const gp_Vec span(point(bend.axisStart), point(bend.axisEnd));
        if (!std::isfinite(span.Magnitude()) || span.Magnitude() <= tolerance ||
            !gp_Dir(span).IsParallel(direction, analysis.angleToleranceRadians) ||
            std::abs(a - b) <= tolerance)
            return {};
        intervals.emplace_back(std::min(a, b), std::max(a, b));
    }
    std::sort(intervals.begin(), intervals.end());
    for (std::size_t i = 1; i < intervals.size(); ++i)
        if (intervals[i].first <= intervals[i - 1].second + tolerance)
            return {}; // Overlapping/touching zones require separate topology support.
    const auto first = origin.Translated(gp_Vec(direction) * intervals.front().first);
    const auto last = origin.Translated(gp_Vec(direction) * intervals.back().second);
    result.axisStart = {first.X(), first.Y(), first.Z()};
    result.axisEnd = {last.X(), last.Y(), last.Z()};
    result.lengthMm = intervals.back().second - intervals.front().first;
    result.trimmedEnds = true;
    result.minimumStraightLengthMm = 0;
    result.straightIntervalMm.reset();
    return result;
}
} // namespace bending::cad
