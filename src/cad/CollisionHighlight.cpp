#include "CollisionHighlight.h"
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <algorithm>
#include <cmath>
namespace bending::cad {
namespace {
constexpr std::size_t maxCentres = 8, maxTrianglesPerPatch = 2048, maxVisitsPerPatch = 80000;
constexpr unsigned maxDepth = 24;
double segmentDistance2(const gp_Pnt &p, const gp_Pnt &a, const gp_Pnt &b) {
    const gp_Vec ab(a, b), ap(a, p);
    const auto length2 = ab.SquareMagnitude();
    const double t = length2 > 1.e-20 ? std::clamp(ap.Dot(ab) / length2, 0., 1.) : 0;
    return p.SquareDistance(a.Translated(ab * t));
}
double triangleDistance2(const gp_Pnt &p, const std::array<gp_Pnt, 3> &v) {
    const auto n = gp_Vec(v[0], v[1]).Crossed(gp_Vec(v[0], v[2]));
    const double n2 = n.SquareMagnitude();
    bool inside = n2 > 1.e-24;
    for (std::size_t i = 0; i < 3 && inside; ++i)
        inside = gp_Vec(v[i], v[(i + 1) % 3]).Crossed(gp_Vec(v[i], p)).Dot(n) >= 0;
    if (inside) {
        const double height = gp_Vec(v[0], p).Dot(n);
        return height * height / n2;
    }
    return std::min({segmentDistance2(p, v[0], v[1]), segmentDistance2(p, v[1], v[2]),
                     segmentDistance2(p, v[2], v[0])});
}
std::vector<gp_Pnt> witnesses(const TopoDS_Shape &overlap, double radiusMm) {
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(overlap, TopAbs_VERTEX, vertices);
    std::vector<gp_Pnt> candidates;
    candidates.reserve(vertices.Extent());
    for (int i = 1; i <= vertices.Extent(); ++i)
        candidates.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))));
    std::vector<gp_Pnt> result;
    // Farthest-first sampling keeps disconnected/long contacts visible at both ends.
    while (!candidates.empty() && result.size() < maxCentres) {
        auto best = candidates.begin();
        double farthest = -1;
        for (auto i = candidates.begin(); i != candidates.end(); ++i) {
            double nearest = result.empty() ? 0 : i->SquareDistance(result.front());
            for (const auto &p : result)
                nearest = std::min(nearest, i->SquareDistance(p));
            if (nearest > farthest) {
                farthest = nearest;
                best = i;
            }
        }
        if (!result.empty() && farthest < radiusMm * radiusMm / 4)
            break;
        result.push_back(*best);
        candidates.erase(best);
    }
    return result;
}
struct LocalMesh {
    CollisionHighlightPatch &patch;
    std::stop_token stop;
    std::size_t triangleLimit = maxTrianglesPerPatch;
    std::size_t visits = 0;
    bool full() const {
        return stop.stop_requested() || patch.vertices.size() >= 3 * triangleLimit ||
               visits >= maxVisitsPerPatch;
    }
    float strength(const gp_Pnt &p) const {
        double distance2 = patch.radiusMm * patch.radiusMm;
        for (const auto &centre : patch.centres)
            distance2 = std::min(distance2, p.SquareDistance(centre));
        const double t = std::clamp(1 - std::sqrt(distance2) / patch.radiusMm, 0., 1.);
        return static_cast<float>(t * t * (3 - 2 * t));
    }
    void triangle(const std::array<gp_Pnt, 3> &v, unsigned depth = 0) {
        if (full())
            return;
        const double r2 = patch.radiusMm * patch.radiusMm;
        const std::array<double, 3> lo{std::min({v[0].X(), v[1].X(), v[2].X()}),
                                       std::min({v[0].Y(), v[1].Y(), v[2].Y()}),
                                       std::min({v[0].Z(), v[1].Z(), v[2].Z()})},
            hi{std::max({v[0].X(), v[1].X(), v[2].X()}), std::max({v[0].Y(), v[1].Y(), v[2].Y()}),
               std::max({v[0].Z(), v[1].Z(), v[2].Z()})};
        if (std::none_of(patch.centres.begin(), patch.centres.end(), [&](const auto &p) {
                const std::array<double, 3> point{p.X(), p.Y(), p.Z()};
                double distance = 0;
                for (std::size_t i = 0; i < 3; ++i) {
                    const double delta = point[i] - std::clamp(point[i], lo[i], hi[i]);
                    distance += delta * delta;
                }
                return distance < r2 && triangleDistance2(p, v) < r2;
            }))
            return;
        ++visits;
        std::array<double, 3> edges{v[0].SquareDistance(v[1]), v[1].SquareDistance(v[2]),
                                    v[2].SquareDistance(v[0])};
        const auto edge =
            static_cast<std::size_t>(std::max_element(edges.begin(), edges.end()) - edges.begin());
        if (depth < maxDepth && edges[edge] > r2 / (triangleLimit >= 512 ? 9 : 4)) {
            const auto a = edge, b = (edge + 1) % 3, c = (edge + 2) % 3;
            const gp_Pnt middle((v[a].XYZ() + v[b].XYZ()) / 2);
            triangle({v[a], middle, v[c]}, depth + 1);
            triangle({middle, v[b], v[c]}, depth + 1);
            return;
        }
        const std::array<float, 3> weights{strength(v[0]), strength(v[1]), strength(v[2])};
        if (*std::max_element(weights.begin(), weights.end()) <= 0)
            return;
        for (std::size_t i = 0; i < 3; ++i)
            patch.vertices.push_back({v[i], weights[i]});
    }
    void shape(const TopoDS_Shape &shape) {
        for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More() && !full(); faces.Next()) {
            const auto face = TopoDS::Face(faces.Current());
            Bnd_Box bounds;
            BRepBndLib::Add(face, bounds);
            if (bounds.IsVoid())
                continue;
            bounds.Enlarge(patch.radiusMm);
            if (std::none_of(patch.centres.begin(), patch.centres.end(),
                             [&](const auto &p) { return !bounds.IsOut(p); }))
                continue;
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(face, location);
            if (mesh.IsNull())
                continue;
            const auto &transform = location.Transformation();
            for (int i = 1; i <= mesh->NbTriangles() && !full(); ++i) {
                int a, b, c;
                mesh->Triangle(i).Get(a, b, c);
                triangle({mesh->Node(a).Transformed(transform),
                          mesh->Node(b).Transformed(transform),
                          mesh->Node(c).Transformed(transform)});
            }
        }
    }
};
} // namespace
void buildCollisionHighlights(PreviewFrame &frame, const std::vector<CollisionEvidence> &evidence,
                              double thicknessMm, std::stop_token stop) {
    if (evidence.empty() || stop.stop_requested())
        return;
    std::vector<CollisionHighlightPatch> pending;
    // User requested four times the original diameter (therefore four times the radius).
    const auto radius = 4 * std::clamp(4 * thicknessMm, 8., 20.);
    for (const auto &item : evidence) {
        if (item.overlap.IsNull())
            continue;
        const auto add = [&](const TopoDS_Shape &region) {
            const auto points = witnesses(region, radius);
            if (!points.empty())
                for (const auto &id : {item.firstId, item.secondId})
                    pending.push_back({id, radius, points, {}});
        };
        bool solid = false;
        for (TopExp_Explorer it(item.overlap, TopAbs_SOLID); it.More(); it.Next()) {
            solid = true;
            add(it.Current());
        }
        if (!solid)
            add(item.overlap);
    }
    // Share the mesh budget between disjoint contacts, so the first one cannot consume it all.
    const auto limit = pending.empty()
                           ? maxTrianglesPerPatch
                           : std::min(maxTrianglesPerPatch,
                                      std::max(std::size_t{1}, std::size_t{8192} / pending.size()));
    for (auto &patch : pending) {
        patch.vertices.reserve(3 * std::min(std::size_t{256}, limit));
        LocalMesh mesh{patch, stop, limit};
        if (patch.bodyId == "sheet") {
            const auto &regions =
                frame.displayShape.IsNull() ? frame.regions : frame.displayRegions;
            for (const auto &region : regions)
                mesh.shape(region);
        } else if (patch.bodyId == "die" || patch.bodyId == "punch")
            mesh.shape(frame.tools[patch.bodyId == "die" ? 0 : 1]);
        else
            for (const auto &component : frame.machine)
                if (component.id == patch.bodyId)
                    mesh.shape(component.shape);
        if (!patch.vertices.empty())
            frame.collisionHighlights.push_back(std::move(patch));
    }
}
} // namespace bending::cad
