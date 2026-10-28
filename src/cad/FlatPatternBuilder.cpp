#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/CadDocument.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace bending::cad {
namespace {
using namespace application;
using Wall = std::array<std::size_t, 2>;
class CadFlatPattern final : public FlatPattern {
  public:
    FlatPatternInfo data;
    TopoDS_Shape display;
    std::vector<TopoDS_Shape> lines;
    const FlatPatternInfo &info() const noexcept override {
        return data;
    }
};
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw FlatPatternError{FlatPatternErrorCode::Cancelled, "Cancelled"};
}
void require(bool condition, const char *message) {
    if (!condition)
        throw FlatPatternError{FlatPatternErrorCode::UnsupportedGeometry, message};
}
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
double area(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p);
    return std::abs(p.Mass());
}
template <class Op>
TopoDS_Shape boolean(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                     std::stop_token stop) {
    cancelled(stop);
    Op op;
    TopTools_ListOfShape args, tools;
    args.Append(a);
    tools.Append(b);
    op.SetArguments(args);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.SetRunParallel(false);
    op.SetFuzzyValue(tolerance);
    op.Build();
    cancelled(stop);
    require(op.IsDone() && !op.HasErrors() && BRepCheck_Analyzer(op.Shape()).IsValid(),
            "Boolean verification failed");
    return op.Shape();
}
TopoDS_Face face(const TopTools_IndexedMapOfShape &faces, std::size_t i) {
    require(i && i <= static_cast<std::size_t>(faces.Extent()), "Invalid face reference");
    return TopoDS::Face(faces(static_cast<int>(i)));
}
gp_Pln plane(const TopTools_IndexedMapOfShape &faces, std::size_t i) {
    BRepAdaptor_Surface s(face(faces, i));
    require(s.GetType() == GeomAbs_Plane, "A wall is not planar");
    return s.Plane();
}
gp_Pnt projected(gp_Pnt p, const gp_Pln &plane) {
    const gp_Vec n(plane.Axis().Direction());
    return p.Translated(-n * gp_Vec(plane.Location(), p).Dot(n));
}
Wall wall(const RecognizedBend &b, std::size_t side) {
    Wall w{b.flangeFaces[side * 2], b.flangeFaces[side * 2 + 1]};
    std::sort(w.begin(), w.end());
    return w;
}
TopoDS_Shape transformed(const TopoDS_Shape &s, gp_Pnt origin, gp_Dir axis, gp_Dir along,
                         double offset = 0) {
    gp_Trsf tr;
    tr.SetTransformation(gp_Ax3(origin, gp_Dir(gp_Vec(axis).Crossed(gp_Vec(along))), axis));
    auto result = BRepBuilderAPI_Transform(s, tr, true).Shape();
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(0, offset, 0));
    return BRepBuilderAPI_Transform(result, shift, true).Shape();
}
BoundsMm bounds(const TopoDS_Shape &s) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, false, false);
    require(!b.IsVoid() && !b.IsOpen(), "Invalid bounds");
    BoundsMm result{};
    b.Get(result.minimum.x, result.minimum.y, result.minimum.z, result.maximum.x, result.maximum.y,
          result.maximum.z);
    return result;
}
TopoDS_Shape extruded(const TopoDS_Face &f, gp_Vec v) {
    auto result = BRepPrimAPI_MakePrism(f, v, true).Shape();
    GProp_GProps p;
    BRepGProp::VolumeProperties(result, p);
    if (p.Mass() < 0)
        result.Reverse();
    require(BRepCheck_Analyzer(result).IsValid(), "Invalid wall prism");
    return result;
}
TopoDS_Shape rectangle(double x0, double x1, double y0, double y1) {
    return BRepBuilderAPI_MakeFace(
               gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))), x0, x1, y0, y1)
        .Face();
}
TopoDS_Shape bendSolid(const RecognizedBend &b, const TopTools_IndexedMapOfShape &faces,
                       double tolerance) {
    const auto f = face(faces, b.innerFace);
    BRepAdaptor_Surface s(f);
    require(s.GetType() == GeomAbs_Cylinder && !b.trimmedEnds,
            "Only complete cylindrical bends are supported");
    double u0, u1, v0, v1;
    BRepTools::UVBounds(f, u0, u1, v0, v1);
    require(std::abs((u1 - u0) - b.angleRadians) < 1e-6 &&
                std::abs((v1 - v0) - b.lengthMm) < tolerance * 20 &&
                std::abs(area(f) - b.innerRadiusMm * (u1 - u0) * (v1 - v0)) <
                    std::max(1e-5, area(f) * 1e-7),
            "A bend has openings, cut edges or an incomplete cylindrical surface");
    const auto cylinder = s.Cylinder();
    const auto p = [&](double u, double offset) {
        auto inner = s.Value(u, v0);
        // Projection onto the axial line, not onto its normal plane.
        const auto axial = cylinder.Location().Translated(
            gp_Vec(cylinder.Axis().Direction()) *
            gp_Vec(cylinder.Location(), inner).Dot(gp_Vec(cylinder.Axis().Direction())));
        return inner.Translated(gp_Vec(axial, inner) * (offset / b.innerRadiusMm));
    };
    const auto arc = [&](double offset) {
        return BRepBuilderAPI_MakeEdge(
                   GC_MakeArcOfCircle(p(u0, offset), p((u0 + u1) / 2, offset), p(u1, offset))
                       .Value())
            .Edge();
    };
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p(u0, 0), p(u0, b.thicknessMm)).Edge());
    wire.Add(arc(b.thicknessMm));
    wire.Add(BRepBuilderAPI_MakeEdge(p(u1, b.thicknessMm), p(u1, 0)).Edge());
    wire.Add(TopoDS::Edge(arc(0).Reversed()));
    return extruded(BRepBuilderAPI_MakeFace(wire.Wire()).Face(),
                    gp_Vec(cylinder.Axis().Direction()) * (v1 - v0));
}
} // namespace
const TopoDS_Shape &OcctFlatPatternAccess::shape(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).display;
}
const std::vector<TopoDS_Shape> &OcctFlatPatternAccess::bendLines(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).lines;
}
FlatPatternResult FlatPatternBuilder::build(DocumentPtr document, FlatPatternRequest request,
                                            std::stop_token stop) const {
    try {
        cancelled(stop);
        if (!core::validUnfoldSettings(request.settings) ||
            (!request.settings.manualK &&
             request.settings.material == core::SheetMaterial::Unknown))
            return FlatPatternError{FlatPatternErrorCode::MissingMaterial,
                                    "Choose a material or set K"};
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        require(native != nullptr, "Unsupported document type");
        const auto &analysis = native->metadata().bendAnalysis;
        require(analysis.interiorContourDiagnostics.empty(),
                "Openings in a bend zone are not yet supported for unfolding");
        require(analysis.status == BendRecognitionStatus::Completed && !analysis.bends.empty() &&
                    analysis.bends.size() <= 32,
                "Requires 1 to 32 recognized straight bends in a single open chain");
        const auto &bends = analysis.bends;
        const double tolerance = std::max(1e-6, analysis.lengthToleranceMm);
        const double t = bends.front().thicknessMm;
        if (request.manualThicknessMm && (!std::isfinite(*request.manualThicknessMm) ||
                                          std::abs(*request.manualThicknessMm - t) > tolerance))
            return FlatPatternError{FlatPatternErrorCode::ThicknessConflict,
                                    "Thickness differs from CAD"};
        auto source = BRepBuilderAPI_Copy(OcctDocumentAccess::shape(*native), true, false).Shape();
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(source, TopAbs_FACE, faces);
        auto axis = gp_Dir(gp_Vec(point(bends.front().axisStart), point(bends.front().axisEnd)));
        std::map<Wall, std::vector<std::size_t>> graph;
        for (std::size_t i = 0; i < bends.size(); ++i) {
            const auto &b = bends[i];
            require(std::abs(b.thicknessMm - t) <= tolerance && !b.trimmedEnds &&
                        axis.IsParallel(gp_Dir(gp_Vec(point(b.axisStart), point(b.axisEnd))), 1e-7),
                    "Requires constant thickness and parallel complete bends");
            graph[wall(b, 0)].push_back(i);
            graph[wall(b, 1)].push_back(i);
        }
        std::vector<Wall> ends;
        for (const auto &[w, neighbours] : graph) {
            require(neighbours.size() <= 2, "Branched sheets are not yet supported");
            if (neighbours.size() == 1)
                ends.push_back(w);
        }
        require(ends.size() == 2, "Closed or disconnected sheet graph");
        std::vector<Wall> walls{ends.front()};
        std::vector<std::size_t> order;
        while (order.size() < bends.size()) {
            const auto &next = graph.at(walls.back());
            const auto it = std::find_if(next.begin(), next.end(), [&](auto i) {
                return std::find(order.begin(), order.end(), i) == order.end();
            });
            require(it != next.end(), "Incomplete or disconnected sheet graph");
            order.push_back(*it);
            walls.push_back(wall(bends[*it], 0) == walls.back() ? wall(bends[*it], 1)
                                                                : wall(bends[*it], 0));
        }
        require(walls.back() == ends.back() && walls.size() == graph.size(),
                "Incomplete sheet coverage");
        auto result = std::make_shared<CadFlatPattern>();
        result->data.settings = request.settings;
        result->data.thicknessMm = t;
        TopoDS_Compound original;
        BRep_Builder builder;
        builder.MakeCompound(original);
        std::vector<TopoDS_Shape> patches;
        std::vector<gp_Dir> directions;
        double y = 0;
        for (std::size_t wi = 0; wi < walls.size(); ++wi) {
            cancelled(stop);
            const auto w = walls[wi];
            const auto pa = plane(faces, w[0]), pb = plane(faces, w[1]);
            require(pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-7) &&
                        std::abs(pa.Distance(pb.Location()) - t) <= tolerance,
                    "Inconsistent wall thickness");
            const gp_Vec n(pa.Axis().Direction());
            builder.Add(original, extruded(face(faces, w[0]),
                                           n * gp_Vec(pa.Location(), pb.Location()).Dot(n)));
            const auto tangent = [&](std::size_t oi) {
                return projected(point(bends[order[oi]].axisStart), pa);
            };
            const auto anchor = tangent(wi ? wi - 1 : 0);
            gp_Dir along(gp_Vec(axis).Crossed(n));
            if (wi && wi < order.size())
                along = gp_Dir(gp_Vec(anchor, tangent(wi)));
            else {
                const auto range = bounds(transformed(face(faces, w[0]), anchor, axis, along));
                const bool positive = range.maximum.y > -range.minimum.y;
                if ((wi == 0 && positive) || (wi > 0 && !positive))
                    along.Reverse();
            }
            const auto flat = transformed(face(faces, w[0]), anchor, axis, along);
            const auto box = bounds(flat);
            double offset = y;
            if (wi == 0) {
                require(box.maximum.y <= tolerance * 20, "First wall crosses its bend rail");
                offset = -box.minimum.y;
                result->data.wallBoundaries.push_back(0);
            } else {
                require(box.minimum.y >= -tolerance * 20, "Wall crosses its bend rail");
            }
            if (wi < order.size() && wi > 0) {
                const double distance = gp_Vec(anchor, tangent(wi)).Magnitude();
                require(distance > tolerance && box.maximum.y <= distance + tolerance * 20,
                        "Wall extends beyond the next bend rail");
                y = offset + distance;
            } else
                y = offset + box.maximum.y;
            patches.push_back(transformed(face(faces, w[0]), anchor, axis, along, offset));
            directions.push_back(along);
            result->data.wallBoundaries.push_back(y);
            if (wi < order.size()) {
                const auto &b = bends[order[wi]];
                const double k = *core::unfoldK(request.settings, b.innerRadiusMm, t);
                const auto ba = core::bendAllowance(b.innerRadiusMm, t, b.angleRadians, k);
                require(ba.has_value(), "Invalid bend parameters");
                double x0 = gp_Vec(gp_Pnt(0, 0, 0), point(b.axisStart)).Dot(gp_Vec(axis));
                double x1 = gp_Vec(gp_Pnt(0, 0, 0), point(b.axisEnd)).Dot(gp_Vec(axis));
                // Each wall's local X has an origin on its bend rail; convert all to global axial
                // X. Applied below to the wall patch as well.
                const double ax = gp_Vec(gp_Pnt(0, 0, 0), anchor).Dot(gp_Vec(axis));
                gp_Trsf shift;
                shift.SetTranslation(gp_Vec(ax, 0, 0));
                patches.back() = BRepBuilderAPI_Transform(patches.back(), shift, true).Shape();
                if (x0 > x1)
                    std::swap(x0, x1);
                patches.push_back(rectangle(x0, x1, y, y + *ba));
                result->data.bends.push_back(
                    {order[wi], y, y + *ba, x0, x1, b.angleRadians, 1, b.innerRadiusMm, k, *ba});
                builder.Add(original, bendSolid(b, faces, tolerance));
                y += *ba;
                result->data.wallBoundaries.push_back(y);
            } else {
                const double ax = gp_Vec(gp_Pnt(0, 0, 0), anchor).Dot(gp_Vec(axis));
                gp_Trsf shift;
                shift.SetTranslation(gp_Vec(ax, 0, 0));
                patches.back() = BRepBuilderAPI_Transform(patches.back(), shift, true).Shape();
            }
        }
        for (std::size_t i = 0; i < order.size(); ++i) {
            const double turn =
                gp_Vec(axis).Dot(gp_Vec(directions[i]).Crossed(gp_Vec(directions[i + 1])));
            require(std::abs(std::acos(std::clamp(directions[i].Dot(directions[i + 1]), -1., 1.)) -
                             bends[order[i]].angleRadians) < 1e-6,
                    "Wall directions do not match the measured bend");
            result->data.bends[i].direction = turn >= 0 ? 1 : -1;
        }
        const double budget = std::max(1e-4, native->metadata().volumeMm3 * 1e-7);
        require(std::abs(volume(original) - volume(source)) <= budget &&
                    volume(boolean<BRepAlgoAPI_Cut>(source, original, tolerance, stop)) <= budget &&
                    volume(boolean<BRepAlgoAPI_Cut>(original, source, tolerance, stop)) <= budget,
                "The recognized walls and bends do not reconstruct the complete original solid");
        for (auto &p : patches) {
            auto f = TopoDS::Face(p);
            const double normal = BRepAdaptor_Surface(f).Plane().Axis().Direction().Z() *
                                  (f.Orientation() == TopAbs_REVERSED ? -1 : 1);
            if (normal < 0)
                f.Reverse();
            p = f;
        }
        TopoDS_Shape flat = patches.front();
        for (std::size_t i = 1; i < patches.size(); ++i) {
            require(area(boolean<BRepAlgoAPI_Common>(flat, patches[i], tolerance, stop)) <= 1e-5,
                    "Flat wall patches overlap");
            flat = boolean<BRepAlgoAPI_Fuse>(flat, patches[i], tolerance, stop);
        }
        ShapeUpgrade_UnifySameDomain unify(flat, true, true, true);
        unify.Build();
        flat = unify.Shape();
        TopTools_IndexedMapOfShape flatFaces;
        TopExp::MapShapes(flat, TopAbs_FACE, flatFaces);
        require(flatFaces.Extent() == 1 && BRepCheck_Analyzer(flat).IsValid(),
                "Flat contour is disconnected or invalid");
        auto box = bounds(flat);
        gp_Trsf origin;
        origin.SetTranslation(gp_Vec(-box.minimum.x, -box.minimum.y, 0));
        flat = BRepBuilderAPI_Transform(flat, origin, true).Shape();
        for (auto &b : result->data.bends) {
            b.minimumX -= box.minimum.x;
            b.maximumX -= box.minimum.x;
            b.firstY -= box.minimum.y;
            b.secondY -= box.minimum.y;
        }
        for (auto &boundary : result->data.wallBoundaries)
            boundary -= box.minimum.y;
        result->display =
            extruded(TopoDS::Face(TopExp_Explorer(flat, TopAbs_FACE).Current()), gp_Vec(0, 0, t));
        result->data.bounds = bounds(result->display);
        BRepMesh_IncrementalMesh mesh(result->display, .08, false, .25, false);
        require(mesh.IsDone(), "Flat preview meshing failed");
        TopoDS_Compound display;
        builder.MakeCompound(display);
        builder.Add(display, result->display);
        for (const auto &b : result->data.bends) {
            auto line =
                BRepBuilderAPI_MakeEdge(gp_Pnt(b.minimumX, (b.firstY + b.secondY) / 2, t + .005),
                                        gp_Pnt(b.maximumX, (b.firstY + b.secondY) / 2, t + .005))
                    .Shape();
            result->lines.push_back(line);
            builder.Add(display, line);
        }
        result->display = display;
        cancelled(stop);
        return FlatPatternPtr(std::move(result));
    } catch (const FlatPatternError &e) {
        return e;
    } catch (const Standard_Failure &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed,
                                e.GetMessageString() ? e.GetMessageString() : "CAD failure"};
    } catch (const std::exception &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed, e.what()};
    }
}
} // namespace bending::cad
