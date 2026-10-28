#include "bending/cad/FlatPatternBuilder.h"
#include "FlatPatternDisplay.h"
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
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
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
    DocumentPtr source;
    FlatPatternInfo data;
    TopoDS_Shape display;
    TopoDS_Shape geometry;
    std::vector<std::vector<PointMm>> contours;
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
                     std::stop_token stop, const char *context = "Boolean verification failed") {
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
    require(op.IsDone() && !op.HasErrors() && BRepCheck_Analyzer(op.Shape()).IsValid(), context);
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
struct WallPlacement {
    gp_Pnt anchor;
    gp_Dir along; // World direction mapped to flat +Y; all walls share axial X.
    double y;
    std::optional<std::size_t> incoming;
};
gp_Pnt flatPoint(gp_Pnt p, const WallPlacement &placement, gp_Dir axis, double zShift) {
    const gp_Vec offset(placement.anchor, p);
    return {gp_Vec(gp_Pnt(0, 0, 0), p).Dot(gp_Vec(axis)),
            offset.Dot(gp_Vec(placement.along)) + placement.y,
            offset.Dot(gp_Vec(axis).Crossed(gp_Vec(placement.along))) + zShift};
}
// A full outer tangency rail must belong to this wall. A local point on one
// side alone cannot prove attachment along the complete axial span.
int materialSide(const TopoDS_Face &wallFace, const gp_Pln &wallPlane, const RecognizedBend &bend,
                 gp_Dir along, double tolerance, std::stop_token stop) {
    const auto first = projected(point(bend.axisStart), wallPlane);
    const auto second = projected(point(bend.axisEnd), wallPlane);
    const auto rail = BRepBuilderAPI_MakeEdge(first, second).Shape();
    const auto common = boolean<BRepAlgoAPI_Common>(rail, BRepTools::OuterWire(wallFace), tolerance,
                                                    stop, "Tangency rail intersection failed");
    GProp_GProps properties;
    BRepGProp::LinearProperties(common, properties, true);
    require(std::abs(properties.Mass() - first.Distance(second)) <=
                std::max(tolerance * 20, bend.lengthMm * 1e-7),
            "A complete tangency rail is not on the outer boundary of its wall");
    const double inset =
        std::max(tolerance * 50, std::min(bend.thicknessMm, bend.innerRadiusMm) * .05);
    int side = 0;
    for (double fraction : {.25, .5, .75}) {
        cancelled(stop);
        const auto center = first.Translated(gp_Vec(first, second) * fraction);
        const auto classify = [&](double sign) {
            return BRepClass_FaceClassifier(
                       wallFace, center.Translated(gp_Vec(along) * (inset * sign)), tolerance)
                .State();
        };
        const auto positive = classify(1), negative = classify(-1);
        const int current = positive == TopAbs_IN && negative == TopAbs_OUT   ? 1
                            : negative == TopAbs_IN && positive == TopAbs_OUT ? -1
                                                                              : 0;
        require(current && (!side || side == current),
                "The material side of a tangency rail is ambiguous");
        side = current;
    }
    return side;
}
TopoDS_Shape flatWall(const TopoDS_Face &f, const WallPlacement &placement, gp_Dir axis) {
    auto patch = transformed(f, placement.anchor, axis, placement.along, placement.y);
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(gp_Vec(gp_Pnt(0, 0, 0), placement.anchor).Dot(gp_Vec(axis)), 0, 0));
    return BRepBuilderAPI_Transform(patch, shift, true).Shape();
}
} // namespace
bool OcctFlatPatternAccess::belongsTo(const FlatPattern &pattern, const Document &document) {
    const auto *native = dynamic_cast<const CadFlatPattern *>(&pattern);
    return native && native->source.get() == &document;
}
const TopoDS_Shape &OcctFlatPatternAccess::shape(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).display;
}
const TopoDS_Shape &OcctFlatPatternAccess::geometry(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).geometry;
}
const std::vector<std::vector<PointMm>> &
OcctFlatPatternAccess::openingContours(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).contours;
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
        const bool cutoutsIgnored =
            native->metadata().closedCutouts.status == ClosedCutoutStatus::Filled;
        const auto &analysis = OcctDocumentAccess::simulationBends(*native);
        require(analysis.interiorContourDiagnostics.empty(),
                "Openings in a bend zone are not yet supported for unfolding");
        require(analysis.status == BendRecognitionStatus::Completed && !analysis.bends.empty() &&
                    analysis.bends.size() <= 32,
                "Requires 1 to 32 recognized straight bends in a connected open sheet");
        const auto &bends = analysis.bends;
        const double tolerance = std::max(1e-6, analysis.lengthToleranceMm);
        const double t = bends.front().thicknessMm;
        if (request.manualThicknessMm && (!std::isfinite(*request.manualThicknessMm) ||
                                          std::abs(*request.manualThicknessMm - t) > tolerance))
            return FlatPatternError{FlatPatternErrorCode::ThicknessConflict,
                                    "Thickness differs from CAD"};
        auto source =
            BRepBuilderAPI_Copy(OcctDocumentAccess::simulationShape(*native), true, false).Shape();
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
        // A connected tree has one more wall than bends. Never drop links to
        // create an arbitrary spanning tree for a closed/disconnected sheet.
        require(graph.size() == bends.size() + 1, "Closed or disconnected sheet graph");
        auto root = graph.begin()->first;
        double rootArea = 0;
        for (const auto &[w, neighbours] : graph) {
            (void)neighbours;
            const double candidate = area(face(faces, w[0]));
            if (candidate > rootArea) {
                root = w;
                rootArea = candidate;
            }
        }
        const auto rootPlane = plane(faces, root[0]), rootOther = plane(faces, root[1]);
        const gp_Vec rootNormal(rootPlane.Axis().Direction());
        const auto toOther =
            rootNormal * gp_Vec(rootPlane.Location(), rootOther.Location()).Dot(rootNormal);
        require(toOther.Magnitude() > tolerance, "Degenerate reference wall");
        std::map<Wall, WallPlacement> placements;
        placements.emplace(root,
                           WallPlacement{projected(gp_Pnt(0, 0, 0), rootPlane),
                                         gp_Dir(toOther.Crossed(gp_Vec(axis))), 0, std::nullopt});
        std::vector<Wall> pending{root};
        std::vector<bool> used(bends.size());
        auto result = std::make_shared<CadFlatPattern>();
        result->source = document;
        result->data.settings = request.settings;
        result->data.thicknessMm = t;
        result->data.cutoutsIgnored = cutoutsIgnored;
        TopoDS_Compound original;
        BRep_Builder builder;
        builder.MakeCompound(original);
        std::vector<TopoDS_Shape> patches;
        std::vector<FlatDisplayRegion> regions;
        for (std::size_t wi = 0; wi < pending.size(); ++wi) {
            cancelled(stop);
            const auto w = pending[wi];
            const auto &placement = placements.at(w);
            const auto pa = plane(faces, w[0]), pb = plane(faces, w[1]);
            require(pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-7) &&
                        std::abs(pa.Axis().Direction().Dot(axis)) < 1e-7 &&
                        std::abs(pa.Distance(pb.Location()) - t) <= tolerance,
                    "Inconsistent wall thickness or axial direction");
            const gp_Vec n(pa.Axis().Direction());
            const auto wallSolid =
                extruded(face(faces, w[0]), n * gp_Vec(pa.Location(), pb.Location()).Dot(n));
            builder.Add(original, wallSolid);
            const double zShift =
                gp_Vec(pa.Location(), pb.Location())
                            .Dot(gp_Vec(axis).Crossed(gp_Vec(placement.along))) < 0
                    ? t
                    : 0;
            const auto wallMap = [placement, axis, zShift](gp_Pnt p) {
                return flatPoint(p, placement, axis, zShift);
            };
            regions.push_back({wallSolid, wallMap});
            patches.push_back(flatWall(face(faces, w[0]), placement, axis));
            const auto wallBox = bounds(patches.back());
            result->data.wallBoundaries.push_back(wallBox.minimum.y);
            result->data.wallBoundaries.push_back(wallBox.maximum.y);
            for (auto index : graph.at(w)) {
                if (placement.incoming == index)
                    continue;
                require(!used[index], "A sheet cycle cannot be unfolded without an explicit seam");
                const auto &b = bends[index];
                const auto child = wall(b, 0) == w ? wall(b, 1) : wall(b, 0);
                require(!placements.contains(child),
                        "A sheet cycle cannot be unfolded without an explicit seam");
                const int outward =
                    -materialSide(face(faces, w[0]), pa, b, placement.along, tolerance, stop);
                const auto childPlane = plane(faces, child[0]);
                const auto childAnchor = projected(point(b.axisStart), childPlane);
                gp_Dir childAlong(gp_Vec(axis).Crossed(gp_Vec(childPlane.Axis().Direction())));
                const int childInside =
                    materialSide(face(faces, child[0]), childPlane, b, childAlong, tolerance, stop);
                if (childInside * outward < 0)
                    childAlong.Reverse();
                require(std::abs(std::acos(std::clamp(placement.along.Dot(childAlong), -1., 1.)) -
                                 b.angleRadians) < 1e-6,
                        "Wall directions do not match the measured bend");
                const double y0 =
                    placement.y + gp_Vec(placement.anchor, projected(point(b.axisStart), pa))
                                      .Dot(gp_Vec(placement.along));
                const double k = *core::unfoldK(request.settings, b.innerRadiusMm, t);
                const auto ba = core::bendAllowance(b.innerRadiusMm, t, b.angleRadians, k);
                require(ba.has_value(), "Invalid bend parameters");
                const double y1 = y0 + outward * *ba;
                placements.emplace(child, WallPlacement{childAnchor, childAlong, y1, index});
                pending.push_back(child);
                used[index] = true;
                double x0 = gp_Vec(gp_Pnt(0, 0, 0), point(b.axisStart)).Dot(gp_Vec(axis));
                double x1 = gp_Vec(gp_Pnt(0, 0, 0), point(b.axisEnd)).Dot(gp_Vec(axis));
                if (x0 > x1)
                    std::swap(x0, x1);
                patches.push_back(rectangle(x0, x1, std::min(y0, y1), std::max(y0, y1)));
                const double turn =
                    gp_Vec(axis).Dot(gp_Vec(placement.along).Crossed(gp_Vec(childAlong)));
                result->data.bends.push_back({index, std::min(y0, y1), std::max(y0, y1), x0, x1,
                                              b.angleRadians, outward * (turn >= 0 ? 1 : -1),
                                              b.innerRadiusMm, k, *ba});
                result->data.wallBoundaries.push_back(y0);
                result->data.wallBoundaries.push_back(y1);
                const auto sector = bendSolid(b, faces, tolerance);
                builder.Add(original, sector);
                const auto center = point(b.axisStart);
                const gp_Dir radial(gp_Vec(center, projected(center, pa)));
                const gp_Dir otherRadial(gp_Vec(center, projected(center, childPlane)));
                const double sign =
                    gp_Vec(axis).Dot(gp_Vec(radial).Crossed(gp_Vec(otherRadial))) >= 0 ? 1 : -1;
                const double zInner =
                    wallMap(center.Translated(gp_Vec(radial) * b.innerRadiusMm)).Z();
                const double zSign =
                    gp_Vec(radial).Dot(gp_Vec(axis).Crossed(gp_Vec(placement.along)));
                regions.push_back(
                    {sector, [center, axis, radial, sign, y0, y1, zInner, zSign, b](gp_Pnt p) {
                         auto r = gp_Vec(center, p);
                         r -= gp_Vec(axis) * r.Dot(gp_Vec(axis));
                         const double angle = std::clamp(
                             std::atan2(sign * gp_Vec(axis).Dot(gp_Vec(radial).Crossed(r)),
                                        gp_Vec(radial).Dot(r)),
                             0., b.angleRadians);
                         return gp_Pnt(gp_Vec(gp_Pnt(0, 0, 0), p).Dot(gp_Vec(axis)),
                                       y0 + (y1 - y0) * angle / b.angleRadians,
                                       zInner + zSign * (r.Magnitude() - b.innerRadiusMm));
                     }});
            }
        }
        require(placements.size() == graph.size() &&
                    std::all_of(used.begin(), used.end(), [](bool b) { return b; }),
                "Incomplete or disconnected sheet graph");
        std::sort(result->data.bends.begin(), result->data.bends.end(),
                  [](const auto &a, const auto &b) { return a.bendIndex < b.bendIndex; });
        const double budget = std::max(1e-4, volume(source) * 1e-7);
        require(std::abs(volume(original) - volume(source)) <= budget,
                "The recognized walls and bends do not reconstruct the complete original solid");
        require(volume(boolean<BRepAlgoAPI_Cut>(source, original, tolerance, stop,
                                                "Source coverage cut failed")) <= budget &&
                    volume(boolean<BRepAlgoAPI_Cut>(original, source, tolerance, stop,
                                                    "Reverse source coverage cut failed")) <=
                        budget,
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
            require(area(boolean<BRepAlgoAPI_Common>(flat, patches[i], tolerance, stop,
                                                     "Flat overlap check failed")) <= 1e-5,
                    "Flat wall patches overlap");
            flat = boolean<BRepAlgoAPI_Fuse>(flat, patches[i], tolerance, stop,
                                             "Flat patch union failed");
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
        std::sort(result->data.wallBoundaries.begin(), result->data.wallBoundaries.end());
        result->data.wallBoundaries.erase(
            std::unique(result->data.wallBoundaries.begin(), result->data.wallBoundaries.end(),
                        [tolerance](double a, double b) { return std::abs(a - b) <= tolerance; }),
            result->data.wallBoundaries.end());
        result->display =
            extruded(TopoDS::Face(TopExp_Explorer(flat, TopAbs_FACE).Current()), gp_Vec(0, 0, t));
        result->geometry = result->display;
        result->data.bounds = bounds(result->display);
        if (cutoutsIgnored) {
            auto cutouts =
                flatCutoutDisplay(OcctDocumentAccess::shape(*native), regions,
                                  gp_Vec(-box.minimum.x, -box.minimum.y, 0), tolerance, stop);
            result->display = std::move(cutouts.surface);
            result->contours = std::move(cutouts.contours);
        } else {
            BRepMesh_IncrementalMesh mesh(result->display, .08, false, .25, false);
            require(mesh.IsDone(), "Flat preview meshing failed");
        }
        TopoDS_Compound display;
        builder.MakeCompound(display);
        builder.Add(display, result->display);
        if (cutoutsIgnored) {
            gp_Trsf top;
            top.SetTranslation(gp_Vec(0, 0, t + .005));
            builder.Add(display,
                        BRepBuilderAPI_Transform(BRepTools::OuterWire(TopoDS::Face(
                                                     TopExp_Explorer(flat, TopAbs_FACE).Current())),
                                                 top, true)
                            .Shape());
            for (const auto &contour : result->contours) {
                BRepBuilderAPI_MakePolygon wire;
                for (std::size_t i = 0; i + 1 < contour.size(); ++i)
                    wire.Add(gp_Pnt(contour[i].x, contour[i].y, t + .005));
                wire.Close();
                require(wire.IsDone(), "Cannot build cutout display outline");
                builder.Add(display, wire.Wire());
            }
        }
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
