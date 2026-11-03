#include "ToolShapeHints.h"
#include "bending/core/UnfoldRule.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Pln.hxx>

namespace bending::cad {
namespace {
gp_Pnt point(application::PointMm p) {
    return {p.x, p.y, p.z};
}
struct Face {
    std::optional<gp_Pln> plane;
    std::vector<gp_Pnt> vertices;
};
} // namespace
std::vector<core::ToolFlangeHint> toolShapeHints(const TopoDS_Shape &shape,
                                                 const application::BendAnalysis &analysis,
                                                 std::stop_token stop) {
    std::vector<core::ToolFlangeHint> hints;
    if (analysis.bends.size() < 2 || analysis.bends.size() > 32)
        return hints;
    try {
        TopTools_IndexedMapOfShape mapped;
        TopExp::MapShapes(shape, TopAbs_FACE, mapped);
        std::vector<Face> faces(static_cast<std::size_t>(mapped.Extent()) + 1);
        for (std::size_t i = 0; i < analysis.bends.size(); ++i) {
            if (stop.stop_requested())
                return {};
            const auto &a = analysis.bends[i];
            const gp_Pnt ca = point(a.axisStart);
            const gp_Vec axis(ca, point(a.axisEnd));
            if (axis.SquareMagnitude() < 1.e-12)
                continue;
            const auto z = axis.Normalized();
            const auto allowance =
                core::bendAllowance(a.innerRadiusMm, a.thicknessMm, a.angleRadians, .5);
            if (!allowance)
                continue;
            for (std::size_t side = 0; side < 2; ++side) {
                const auto id = a.flangeFaces[side * 2]; // Recognizer pairs inner/outer surfaces.
                if (!id || id >= faces.size())
                    continue;
                auto &shared = faces[id];
                if (!shared.plane) {
                    BRepAdaptor_Surface surface(TopoDS::Face(mapped(static_cast<int>(id))));
                    if (surface.GetType() != GeomAbs_Plane)
                        continue;
                    shared.plane = surface.Plane();
                }
                const auto &plane = *shared.plane;
                auto normal = gp_Vec(plane.Axis().Direction());
                const double distance = gp_Vec(plane.Location(), ca).Dot(normal);
                if (std::abs(std::abs(distance) - a.innerRadiusMm) >
                        std::max(1.e-4, analysis.lengthToleranceMm * 4) ||
                    std::abs(normal.Dot(z)) > 1.e-6)
                    continue;
                if (distance < 0)
                    normal.Reverse();
                const auto origin = ca.Translated(normal * -a.innerRadiusMm);
                for (std::size_t j = 0; j < analysis.bends.size(); ++j) {
                    if (i == j || stop.stop_requested())
                        continue;
                    const auto &b = analysis.bends[j];
                    const auto sharedAt = std::find(b.flangeFaces.begin(), b.flangeFaces.end(), id);
                    if (sharedAt == b.flangeFaces.end())
                        continue;
                    const auto cb = point(b.axisStart);
                    const gp_Vec otherAxis(cb, point(b.axisEnd));
                    if (otherAxis.SquareMagnitude() < 1.e-12 ||
                        z.Crossed(otherAxis.Normalized()).Magnitude() > 1.e-6)
                        continue;
                    const auto projection = [&](gp_Pnt p) { return gp_Vec(ca, p).Dot(z); };
                    const double za = projection(cb), zb = projection(point(b.axisEnd));
                    if (std::min(axis.Magnitude(), std::max(za, zb)) -
                            std::max(0., std::min(za, zb)) <=
                        analysis.lengthToleranceMm * 4)
                        continue;
                    auto u = gp_Vec(origin, cb);
                    u -= z * u.Dot(z) + normal * u.Dot(normal);
                    if (u.SquareMagnitude() < 1.e-10)
                        continue;
                    u.Normalize();
                    const double sign = u.Crossed(normal).Dot(z) < 0 ? -1 : 1;
                    const auto otherSide =
                        1 - static_cast<std::size_t>(sharedAt - b.flangeFaces.begin()) / 2;
                    core::ToolFlangeHint hint{i, j, {}};
                    bool complete = true;
                    std::optional<gp_Vec> along;
                    for (std::size_t skin = 0; skin < 2 && complete; ++skin) {
                        const auto faceId = b.flangeFaces[otherSide * 2 + skin];
                        if (!faceId || faceId >= faces.size()) {
                            complete = false;
                            break;
                        }
                        auto &face = faces[faceId];
                        if (face.vertices.empty()) {
                            const auto native = TopoDS::Face(mapped(static_cast<int>(faceId)));
                            BRepAdaptor_Surface surface(native);
                            if (surface.GetType() != GeomAbs_Plane) {
                                complete = false;
                                break;
                            }
                            face.plane = surface.Plane();
                            for (TopExp_Explorer vertex(native, TopAbs_VERTEX); vertex.More();
                                 vertex.Next())
                                face.vertices.push_back(
                                    BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current())));
                        }
                        if (!face.plane ||
                            std::abs(gp_Vec(face.plane->Axis().Direction()).Dot(z)) > 1.e-6 ||
                            face.vertices.empty()) {
                            complete = false;
                            break;
                        }
                        const auto direction =
                            z.Crossed(gp_Vec(face.plane->Axis().Direction())).Normalized();
                        if (!along)
                            along = direction;
                        else if (along->Crossed(direction).Magnitude() > 1.e-6) {
                            complete = false;
                            break;
                        }
                        const auto limits = std::minmax_element(
                            face.vertices.begin(), face.vertices.end(), [&](gp_Pnt p, gp_Pnt q) {
                                return gp_Vec(origin, p).Dot(*along) <
                                       gp_Vec(origin, q).Dot(*along);
                            });
                        const auto project = [&](gp_Pnt p) {
                            const auto v = gp_Vec(origin, p);
                            return core::DrawingPoint{sign * (v.Dot(u) + *allowance / 2),
                                                      v.Dot(normal)};
                        };
                        hint.section[skin ? 3 : 0] = project(*limits.first);
                        hint.section[skin ? 2 : 1] = project(*limits.second);
                    }
                    if (complete)
                        hints.push_back(hint);
                }
            }
        }
    } catch (const Standard_Failure &) {
        return {}; // Optional ranking cannot turn a successfully imported part into a failure.
    }
    return hints;
}
} // namespace bending::cad
