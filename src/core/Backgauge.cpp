#include "bending/core/Backgauge.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace bending::core {
double minimumGaugeSpanMm(const BackgaugeSetup &s) noexcept {
    return s.allowOffsetContacts ? s.minimumSpacingMm
                                 : std::max(s.minimumSpacingMm, s.fingerWidthMm);
}
bool validBackgaugeSetup(const BackgaugeSetup &s) noexcept {
    const auto range = [](double a, double b) {
        return std::isfinite(a) && std::isfinite(b) && a >= -10000 && b <= 10000 && a < b;
    };
    return range(s.xMinMm, s.xMaxMm) && s.xMinMm >= 0 && range(s.rMinMm, s.rMaxMm) &&
           range(s.zMinMm, s.zMaxMm) && std::isfinite(s.fingerWidthMm) && s.fingerWidthMm >= 1 &&
           s.fingerWidthMm <= 1000 && std::isfinite(s.minimumSpacingMm) &&
           s.minimumSpacingMm >= 0 && s.minimumSpacingMm <= 10000 && std::isfinite(s.retractMm) &&
           s.retractMm >= 0 && s.retractMm <= 500;
}
namespace {
using Interval = std::array<double, 2>;
std::optional<Interval> contactInterval(const GaugeContactPatch &p, double y, double hy,
                                        double hz) {
    const auto &v = p.vertices;
    const double area =
        (v[1].y - v[0].y) * (v[2].z - v[0].z) - (v[1].z - v[0].z) * (v[2].y - v[0].y);
    if (!std::isfinite(area) || std::abs(area) < 1.e-9)
        return {};
    const double sign = area > 0 ? 1 : -1;
    Interval result{-std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto a = v[i], b = v[(i + 1) % 3];
        const double dy = b.y - a.y, dz = b.z - a.z;
        const double coefficient = sign * dy;
        const double rhs =
            std::abs(dy) * hz + std::abs(dz) * hy + sign * (dy * a.z + dz * (y - a.y));
        if (std::abs(coefficient) < 1.e-12) {
            if (rhs > 1.e-9)
                return {};
        } else if (coefficient > 0)
            result[0] = std::max(result[0], rhs / coefficient);
        else
            result[1] = std::min(result[1], rhs / coefficient);
    }
    return result[0] <= result[1] ? std::optional{result} : std::nullopt;
}
} // namespace
std::vector<BackgaugePlacement> backgaugeCandidates(std::span<const GaugeContactPatch> patches,
                                                    const BackgaugeSetup &setup, double thickness,
                                                    double centre, std::stop_token stop) {
    std::vector<BackgaugePlacement> result;
    if (!validBackgaugeSetup(setup) || !std::isfinite(thickness) || thickness <= 0 ||
        !std::isfinite(centre))
        return result;
    // A small finite contact patch, not a full-width clamping/stability certificate.
    const double hy = std::min(thickness / 4, .5), hz = std::min(setup.fingerWidthMm / 2, 1.);
    const double maximumFaceHeight = std::max({backgaugeHeightsMm[1] - backgaugeHeightsMm[0],
                                               backgaugeHeightsMm[2] - backgaugeHeightsMm[1],
                                               backgaugeHeightsMm[3] - backgaugeHeightsMm[2]});
    struct Plane {
        double x;
        GaugeReferenceKind kind;
        std::vector<const GaugeContactPatch *> patches;
    };
    std::vector<Plane> planes;
    for (const auto &p : patches) {
        if (stop.stop_requested())
            return {};
        if (!std::isfinite(p.xMm) || p.xMm <= 0 ||
            !std::all_of(p.vertices.begin(), p.vertices.end(),
                         [](auto v) { return std::isfinite(v.y) && std::isfinite(v.z); }))
            continue;
        auto it = std::find_if(planes.begin(), planes.end(), [&](const auto &g) {
            return g.kind == p.kind && std::abs(g.x - p.xMm) < 1.e-6;
        });
        if (it == planes.end()) {
            planes.push_back({p.xMm, p.kind, {}});
            it = planes.end() - 1;
        }
        it->patches.push_back(&p);
    }
    for (const auto &plane : planes) {
        // Die top is Y=0 in the common press frame. This positions the finger,
        // never the sheet. A 90-degree wall retains its own reference height.
        const bool raisedEdge = plane.kind == GaugeReferenceKind::StraightEdge;
        if (raisedEdge && thickness + 1.e-6 < backgaugeMinimumEdgeThicknessMm)
            continue;
        std::vector<double> ys;
        for (const auto *p : plane.patches) {
            double y = (p->vertices[0].y + p->vertices[1].y + p->vertices[2].y) / 3;
            if (raisedEdge)
                y = std::clamp(y, backgaugeEdgeClearanceMm + hy,
                               backgaugeEdgeClearanceMm + maximumFaceHeight - hy);
            ys.push_back(y);
        }
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end(),
                             [](double a, double b) { return std::abs(a - b) < 1.e-6; }),
                 ys.end());
        // Bound exceptionally tessellated inputs without sampling every millimetre.
        if (ys.size() > 64) {
            const auto all = ys;
            ys.clear();
            for (std::size_t i = 0; i < 64; ++i)
                ys.push_back(all[i * (all.size() - 1) / 63]);
        }
        for (const auto y : ys) {
            if (stop.stop_requested())
                return {};
            std::vector<Interval> intervals;
            for (const auto *p : plane.patches)
                if (auto interval = contactInterval(*p, y, hy, hz)) {
                    (*interval)[0] = std::max((*interval)[0], centre + setup.zMinMm);
                    (*interval)[1] = std::min((*interval)[1], centre + setup.zMaxMm);
                    if ((*interval)[0] <= (*interval)[1])
                        intervals.push_back(*interval);
                }
            if (intervals.empty())
                continue;
            const double left =
                std::min_element(intervals.begin(), intervals.end(), [](auto a, auto b) {
                    return a[0] < b[0];
                })->at(0);
            const double right =
                std::max_element(intervals.begin(), intervals.end(), [](auto a, auto b) {
                    return a[1] < b[1];
                })->at(1);
            struct Contacts {
                Interval contacts, centres;
            };
            std::vector<Contacts> choices;
            if (right - left >= std::max(setup.minimumSpacingMm, setup.fingerWidthMm))
                choices.push_back({{left, right}, {left, right}});
            if (setup.allowOffsetContacts) {
                const double contactSpan = right - left;
                if (contactSpan >= std::max(setup.minimumSpacingMm, 2 * hz + 1.e-6) &&
                    contactSpan < setup.fingerWidthMm) {
                    const double gap = std::min(1., (contactSpan - 2 * hz) / 2);
                    const double shift = (setup.fingerWidthMm + gap - contactSpan) / 2;
                    const Interval centres{left - shift, right + shift};
                    if (centres[0] >= centre + setup.zMinMm && centres[1] <= centre + setup.zMaxMm)
                        choices.push_back({{left, right}, centres});
                }
            }
            for (const auto level : {1U, 0U, 2U}) {
                const double x = plane.x - backgaugeOffsetsMm[level];
                const double r =
                    raisedEdge
                        ? backgaugeEdgeClearanceMm - backgaugeHeightsMm[level]
                        : y - (backgaugeHeightsMm[level] + backgaugeHeightsMm[level + 1]) / 2;
                if (y - hy < r + backgaugeHeightsMm[level] - 1.e-9 ||
                    y + hy > r + backgaugeHeightsMm[level + 1] + 1.e-9)
                    continue;
                if (x < setup.xMinMm || x > setup.xMaxMm || r < setup.rMinMm || r > setup.rMaxMm)
                    continue;
                for (const auto &choice : choices)
                    result.push_back(
                        {plane.kind, level, x, r, choice.centres, plane.x, y, choice.contacts});
            }
        }
    }
    const auto rank = [](const auto &p) {
        return std::tuple{p.kind,
                          p.level == 1   ? 0
                          : p.level == 0 ? 1
                                         : 2,
                          -(p.contactZmm[1] - p.contactZmm[0]),
                          p.xMm,
                          p.rMm,
                          std::abs(p.zMm[1] - p.zMm[0]),
                          p.zMm[0],
                          p.zMm[1],
                          p.contactZmm[0],
                          p.contactZmm[1]};
    };
    std::stable_sort(result.begin(), result.end(),
                     [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    std::vector<BackgaugePlacement> shortlist;
    std::set<std::pair<GaugeReferenceKind, std::size_t>> families;
    for (const auto &p : result)
        if (families.emplace(p.kind, p.level).second && shortlist.size() < 12)
            shortlist.push_back(p);
    for (const auto &p : result) {
        if (shortlist.size() >= 12)
            break;
        if (std::find(shortlist.begin(), shortlist.end(), p) == shortlist.end())
            shortlist.push_back(p);
    }
    std::stable_sort(shortlist.begin(), shortlist.end(),
                     [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
    return shortlist;
}
} // namespace bending::core
