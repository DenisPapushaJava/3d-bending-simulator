#include "ToolShapeRanking.h"
#include "ToolGeometryInternal.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>

namespace bending::core {
namespace {
std::optional<std::vector<DrawingPoint>> polygon(const ToolDrawing &drawing, DrawingPoint origin) {
    std::vector<DrawingPoint> result;
    for (const auto &e : drawing.edges) {
        const auto count =
            e.center ? std::max(1, static_cast<int>(
                                       std::ceil(std::abs(e.sweepRadians) * 36 / std::numbers::pi)))
                     : 1;
        if (result.size() + static_cast<std::size_t>(count) > 4096)
            return {};
        const double start =
            e.center ? std::atan2(e.start.y - e.center->y, e.start.x - e.center->x) : 0;
        const double radius =
            e.center ? std::hypot(e.start.x - e.center->x, e.start.y - e.center->y) : 0;
        for (int i = 0; i < count; ++i) {
            const double angle = start + e.sweepRadians * i / count;
            const DrawingPoint p = e.center ? DrawingPoint{e.center->x + radius * std::cos(angle),
                                                           e.center->y + radius * std::sin(angle)}
                                            : e.start;
            result.push_back({p.x - origin.x, p.y - origin.y});
        }
    }
    return result;
}
struct ContourEdge {
    DrawingPoint a, b;
    double dx, dy, squaredLength, contactTolerance;
};
struct WorkingContour {
    std::vector<ContourEdge> edges;
    std::size_t vertexCount;
    DrawingPoint minimum, maximum;
};

std::shared_ptr<const WorkingContour> prepareContour(const ToolDrawing &drawing,
                                                     DrawingPoint origin) {
    const auto points = polygon(drawing, origin);
    if (!points || points->empty())
        return {};
    WorkingContour contour{{}, points->size(), points->front(), points->front()};
    contour.edges.reserve(points->size());
    for (std::size_t i = 0; i < points->size(); ++i) {
        const auto a = (*points)[i], b = (*points)[(i + 1) % points->size()];
        contour.minimum = {std::min(contour.minimum.x, a.x), std::min(contour.minimum.y, a.y)};
        contour.maximum = {std::max(contour.maximum.x, a.x), std::max(contour.maximum.y, a.y)};
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length = dx * dx + dy * dy;
        if (length >= 1.e-18)
            contour.edges.push_back({a, b, dx, dy, length, 1.e-6 * std::hypot(dx, dy)});
    }
    return std::make_shared<const WorkingContour>(std::move(contour));
}

// The callers validate the preset/assignment first. Only inputs that change the
// working section belong here. Stock, placement and workpiece data live elsewhere.
std::shared_ptr<const WorkingContour> workingContour(const ToolPreset &tool, std::size_t groove) {
    if (!tool.parametersConfirmed || !tool.drawing || tool.drawing->edges.size() > 2048)
        return {};
    const auto &drawing = *tool.drawing;
    const bool punch = tool.kind == ToolKind::Punch;
    const auto settings =
        punch ? std::optional<DieGroove>{{tool.angleRadians, 0}} : dieGroove(tool, groove);
    if (!settings)
        return {};
    const double size = punch ? tool.noseRadiusMm.value_or(0) : tool.openingsMm[groove];
    struct Entry {
        ToolKind kind;
        DieGroove settings;
        double size;
        ToolDrawing drawing;
        std::shared_ptr<const WorkingContour> contour;
    };
    static std::mutex mutex;
    static std::list<Entry> cache;
    const auto matches = [&](const Entry &e) {
        return e.kind == tool.kind && e.settings == *settings && e.size == size &&
               e.drawing.sha256 == drawing.sha256 && e.drawing.millimeters == drawing.millimeters &&
               e.drawing.edges == drawing.edges;
    };
    // Hold the lock only for lookup/publication. Concurrent misses can build
    // independently, and readers retain immutable contours after eviction.
    const auto find = [&]() {
        const auto it = std::find_if(cache.begin(), cache.end(), matches);
        if (it != cache.end())
            cache.splice(cache.begin(), cache, it);
        return it != cache.end();
    };
    {
        const std::lock_guard lock(mutex);
        if (find())
            return cache.front().contour;
    }
    std::shared_ptr<const WorkingContour> contour;
    if (punch) {
        if (const auto geometry = workingPunchGeometry(tool))
            contour = prepareContour(drawing, geometry->punchTip);
    } else if (const auto active = activeDieDrawing(tool, groove)) {
        if (const auto geometry = workingDieGeometry(tool, groove, *active))
            contour = prepareContour(*active, geometry->dieTopCentre);
    }
    const std::lock_guard lock(mutex);
    if (find())
        return cache.front().contour;
    cache.push_front({tool.kind, *settings, size, drawing, contour});
    constexpr std::size_t maximumCachedContours = 128;
    if (cache.size() > maximumCachedContours)
        cache.pop_back();
    return contour;
}

bool inside(const WorkingContour &contour, DrawingPoint p) {
    bool result = false;
    for (const auto &[a, b, dx, dy, squaredLength, contactTolerance] : contour.edges) {
        const double dot = (p.x - a.x) * dx + (p.y - a.y) * dy;
        if (dot >= 0 && dot <= squaredLength &&
            std::abs(dx * (p.y - a.y) - dy * (p.x - a.x)) <= contactTolerance)
            return false; // Intended/tangential contact is not a body-interior sample.
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
            result = !result;
    }
    return result;
}
DrawingPoint mix(DrawingPoint a, DrawingPoint b, double f) {
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

using Samples = std::array<DrawingPoint, 51>;
using SideMasks = std::array<std::uint64_t, 2>;
struct ShapeSamples {
    std::vector<Samples> points;
    std::vector<std::size_t> neighbourPairs;
    std::size_t pairCount = 0;
};
std::optional<ShapeSamples> prepareSamples(std::span<const ToolFlangeHint> hints, double thickness,
                                           std::stop_token stop) {
    if (hints.empty() || hints.size() > 256 || !std::isfinite(thickness) || thickness <= 0 ||
        stop.stop_requested())
        return {};
    ShapeSamples result;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> pairs;
    for (const auto &hint : hints) {
        if (stop.stop_requested() || hint.bendIndex == hint.neighbourBendIndex ||
            !std::all_of(hint.section.begin(), hint.section.end(), [](auto p) {
                return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= 1.e5 &&
                       std::abs(p.y) <= 1.e5;
            }))
            return {};
        double area = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto p = hint.section[i], q = hint.section[(i + 1) % 4];
            area += p.x * q.y - q.x * p.y;
        }
        if (std::abs(area) < 1.e-8)
            return {};
        auto &points = result.points.emplace_back();
        std::size_t sample = 0;
        for (unsigned i = 0; i <= 16; ++i)
            for (unsigned j = 0; j <= 2; ++j)
                points[sample++] = mix(mix(hint.section[0], hint.section[1], i / 16.),
                                       mix(hint.section[3], hint.section[2], i / 16.), j / 2.);
        const auto key = std::minmax(hint.bendIndex, hint.neighbourBendIndex);
        result.neighbourPairs.push_back(pairs.try_emplace(key, pairs.size()).first->second);
    }
    result.pairCount = pairs.size();
    return result;
}

struct Occupancy {
    std::size_t contourSize;
    std::vector<SideMasks> masks;
};
std::optional<Occupancy> sampleContour(const WorkingContour &contour, const ShapeSamples &samples,
                                       double yShift, std::stop_token stop) {
    if (contour.vertexCount * samples.points.size() * 102 > 2000000)
        return {};
    Occupancy result{contour.vertexCount, {}};
    result.masks.reserve(samples.points.size());
    for (const auto &points : samples.points) {
        if (stop.stop_requested())
            return {};
        auto &masks = result.masks.emplace_back();
        for (std::size_t side = 0; side < masks.size(); ++side)
            for (std::size_t i = 0; i < points.size(); ++i) {
                const DrawingPoint p{points[i].x * (side == 0 ? -1 : 1), points[i].y + yShift};
                if (p.x >= contour.minimum.x && p.x <= contour.maximum.x &&
                    p.y >= contour.minimum.y && p.y <= contour.maximum.y && inside(contour, p))
                    masks[side] |= std::uint64_t{1} << i;
            }
    }
    return result;
}

std::optional<double> combinedRisk(const ShapeSamples &samples, const Occupancy &punch,
                                   const Occupancy &die, bool reversed) {
    if ((punch.contourSize + die.contourSize) * samples.points.size() * 102 > 2000000)
        return {};
    std::array<int, 256> lowest{};
    std::fill_n(lowest.begin(), samples.pairCount, 51);
    for (std::size_t i = 0; i < samples.points.size(); ++i) {
        const auto &p = punch.masks[i], &d = die.masks[i];
        const auto first = std::popcount(p[reversed ? 1 : 0] | d[0]);
        const auto second = std::popcount(p[reversed ? 0 : 1] | d[1]);
        auto &best = lowest[samples.neighbourPairs[i]];
        best = std::min({best, first, second});
    }
    return *std::max_element(lowest.begin(), lowest.begin() + samples.pairCount) / 51.;
}

std::optional<Occupancy> punchOccupancy(const ToolPreset &punch, const ShapeSamples &samples,
                                        std::stop_token stop) {
    const auto contour = workingContour(punch, 0);
    return contour ? sampleContour(*contour, samples, 0, stop) : std::nullopt;
}
std::optional<Occupancy> dieOccupancy(const ToolPreset &die, std::size_t groove,
                                      const ShapeSamples &samples, double thickness,
                                      std::stop_token stop) {
    const auto contour = workingContour(die, groove);
    return contour ? sampleContour(*contour, samples, thickness, stop) : std::nullopt;
}
struct ToolFlangeOccupancy {
    ShapeSamples samples;
    Occupancy punch, die;
};
std::optional<ToolFlangeOccupancy> flangeOccupancy(const ToolAssignment &a,
                                                   std::span<const ToolFlangeHint> hints,
                                                   double thickness, std::stop_token stop) {
    if (!a.useDrawingGeometry || !a.punch.drawing || !a.die.drawing ||
        a.punch.drawing->edges.size() > 2048 || a.die.drawing->edges.size() > 2048)
        return {};
    auto samples = prepareSamples(hints, thickness, stop);
    if (!samples || !toolParameters(a))
        return {};
    auto punch = punchOccupancy(a.punch, *samples, stop);
    auto die = dieOccupancy(a.die, a.groove, *samples, thickness, stop);
    if (!punch || !die || (punch->contourSize + die->contourSize) * hints.size() * 102 > 2000000)
        return {};
    return ToolFlangeOccupancy{std::move(*samples), std::move(*punch), std::move(*die)};
}
} // namespace

std::optional<double> toolShapeRisk(const ToolAssignment &a, std::span<const ToolFlangeHint> hints,
                                    double thickness, std::stop_token stop) {
    const auto data = flangeOccupancy(a, hints, thickness, stop);
    return data ? combinedRisk(data->samples, data->punch, data->die, a.punchReversed)
                : std::nullopt;
}

void evaluateToolCandidateShapes(const ToolCatalog &catalog, const ToolSelectionRequest &request,
                                 std::vector<ToolCandidate> &candidates, std::stop_token stop) {
    if (candidates.empty())
        return;
    const auto samples = prepareSamples(request.shapeHints, request.thicknessMm, stop);
    if (!samples)
        return;
    // Indexed by profile and active groove. Engaged entries remember unavailable
    // geometry too. Reversing the punch only swaps its two workpiece-side masks.
    std::vector<std::optional<std::optional<Occupancy>>> punches(catalog.size());
    std::map<std::pair<std::size_t, std::size_t>, std::optional<Occupancy>> dies;
    for (auto &candidate : candidates) {
        if (stop.stop_requested())
            return;
        const auto &p = catalog[candidate.punch], &d = catalog[candidate.die];
        const auto groove = candidate.groove;
        const auto length = [](const ToolPreset &preset,
                               const std::shared_ptr<const ToolAssembly> &assembly) {
            if (!assembly)
                return toolLength(preset, {});
            return preset.segments ? assemblyLength(*preset.segments, *assembly) : std::nullopt;
        };
        const auto pl = length(p, candidate.punchAssembly), dl = length(d, candidate.dieAssembly);
        if (!pl || !dl ||
            !validPressTools({d.openingsMm[groove], dieGroove(d, groove)->angleRadians,
                              p.angleRadians, *p.noseRadiusMm, *dl, 10, *pl}))
            continue;
        auto &punch = punches[candidate.punch];
        if (!punch)
            punch.emplace(punchOccupancy(p, *samples, stop));
        if (!*punch)
            continue;
        const auto [die, inserted] = dies.try_emplace({candidate.die, groove});
        if (inserted)
            die->second = dieOccupancy(d, groove, *samples, request.thicknessMm, stop);
        if (!die->second)
            continue;
        const auto normal = combinedRisk(*samples, **punch, *die->second, false);
        const auto reversed = combinedRisk(*samples, **punch, *die->second, true);
        if (normal && reversed)
            candidate.shapeRisk = std::min(*normal, *reversed);
    }
}
} // namespace bending::core
