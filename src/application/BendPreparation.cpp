#include "bending/application/BendPreparation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <utility>

namespace bending::application {
namespace {
bool finite(PointMm point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
double distance(PointMm a, PointMm b) {
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}
BendAnchor anchorOf(const RecognizedBend &bend) {
    return {bend.axisStart,   bend.axisEnd,  bend.angleRadians, bend.innerRadiusMm,
            bend.thicknessMm, bend.lengthMm, bend.trimmedEnds,  bend.minimumStraightLengthMm};
}
bool matches(const BendAnchor &a, const RecognizedBend &b, double lengthTolerance,
             double angleTolerance) {
    const auto close = [lengthTolerance](double x, double y) {
        return std::abs(x - y) <= lengthTolerance;
    };
    const bool endpoints = (distance(a.axisStart, b.axisStart) <= lengthTolerance &&
                            distance(a.axisEnd, b.axisEnd) <= lengthTolerance) ||
                           (distance(a.axisStart, b.axisEnd) <= lengthTolerance &&
                            distance(a.axisEnd, b.axisStart) <= lengthTolerance);
    return endpoints && std::abs(a.angleRadians - b.angleRadians) <= angleTolerance &&
           close(a.innerRadiusMm, b.innerRadiusMm) && close(a.thicknessMm, b.thicknessMm) &&
           close(a.lengthMm, b.lengthMm) && a.trimmedEnds == b.trimmedEnds &&
           close(a.minimumStraightLengthMm, b.minimumStraightLengthMm);
}
} // namespace
bool validBendPreparations(const std::vector<BendPreparation> &entries) {
    if (entries.size() > 512) {
        return false;
    }
    std::set<std::string> ids;
    for (const auto &entry : entries) {
        const auto &a = entry.anchor;
        const bool validId = !entry.id.empty() && entry.id.size() <= 64 &&
                             std::all_of(entry.id.begin(), entry.id.end(), [](char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                        (c >= '0' && c <= '9') || c == '-' || c == '_';
                             });
        if (!validId || !ids.insert(entry.id).second || !finite(a.axisStart) ||
            !finite(a.axisEnd) || !std::isfinite(a.angleRadians) || a.angleRadians <= 0.0 ||
            a.angleRadians >= std::numbers::pi || !std::isfinite(a.innerRadiusMm) ||
            a.innerRadiusMm <= 0.0 || !std::isfinite(a.thicknessMm) || a.thicknessMm <= 0.0 ||
            !std::isfinite(a.lengthMm) || a.lengthMm <= 0.0 ||
            !std::isfinite(a.minimumStraightLengthMm) || a.minimumStraightLengthMm <= 0.0 ||
            a.minimumStraightLengthMm > a.lengthMm + 1.0e-5 ||
            !std::isfinite(distance(a.axisStart, a.axisEnd)) ||
            distance(a.axisStart, a.axisEnd) <= 0.0 ||
            (entry.review != BendReview::Pending && entry.review != BendReview::Confirmed &&
             entry.review != BendReview::Excluded) ||
            (entry.target && !core::validBendTarget(*entry.target)) ||
            (entry.tools && !core::validPressTools(*entry.tools))) {
            return false;
        }
    }
    return true;
}
BendBindings restoreBendPreparations(const BendAnalysis &analysis,
                                     std::vector<BendPreparation> &entries) {
    BendBindings result;
    result.byRecognizedBend.resize(analysis.bends.size());
    std::vector<std::vector<std::size_t>> candidates(entries.size());
    std::vector<std::size_t> counts(analysis.bends.size());
    // Bound by recognizer limits; never widen a malformed/unsupported analysis tolerance.
    const auto lengthTolerance = std::clamp(analysis.lengthToleranceMm, 1.0e-5, 0.01);
    const auto angleTolerance = 1.0e-5;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        for (std::size_t b = 0; b < analysis.bends.size(); ++b) {
            if (matches(entries[e].anchor, analysis.bends[b], lengthTolerance, angleTolerance)) {
                candidates[e].push_back(b);
                ++counts[b];
            }
        }
    }
    for (std::size_t e = 0; e < entries.size(); ++e) {
        if (candidates[e].size() == 1 && counts[candidates[e][0]] == 1) {
            result.byRecognizedBend[candidates[e][0]] = e;
        } else {
            result.unresolved.push_back(e);
        }
    }
    std::set<std::string> ids;
    for (const auto &entry : entries) {
        ids.insert(entry.id);
    }
    std::size_t nextId = 1;
    for (std::size_t b = 0; b < analysis.bends.size(); ++b) {
        if (counts[b] != 0 || entries.size() >= 512) {
            continue;
        }
        std::string id;
        do {
            id = "bend-" + std::to_string(nextId++);
        } while (ids.contains(id));
        ids.insert(id);
        result.byRecognizedBend[b] = entries.size();
        entries.push_back({std::move(id), anchorOf(analysis.bends[b]), BendReview::Pending, {}});
    }
    return result;
}
} // namespace bending::application
