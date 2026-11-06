#include "bending/cad/FlatPatternBuilder.h"
#include "BendSectorOwnership.h"
#include "FlatPatternDisplay.h"
#include "FlatWallRegions.h"
#include "ModelMeasurements.h"
#include "RouteGeometry.h"
#include "SheetGeometry.h"
#include "SheetOrientation.h"
#include "TrimmedBend.h"
#include "bending/cad/CadDocument.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/simulation/SingleBend.h"
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
#include <TopoDS_Iterator.hxx>
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
    SheetLayout layout;
    SheetOrientationVisual orientation;
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
gp_Trsf placementMap(gp_Pnt source, gp_Dir axis, gp_Dir across, gp_Pnt target, gp_Dir flatAxis,
                     gp_Dir flatAcross) {
    const gp_Vec normal = gp_Vec(axis).Crossed(gp_Vec(across));
    const gp_Vec flatNormal = gp_Vec(flatAxis).Crossed(gp_Vec(flatAcross));
    double m[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = flatAxis.Coord(i + 1) * axis.Coord(j + 1) +
                      flatAcross.Coord(i + 1) * across.Coord(j + 1) +
                      flatNormal.Coord(i + 1) * normal.Coord(j + 1);
    const auto shift = [&](int i) {
        return target.Coord(i + 1) - m[i][0] * source.X() - m[i][1] * source.Y() -
               m[i][2] * source.Z();
    };
    gp_Trsf result;
    result.SetValues(m[0][0], m[0][1], m[0][2], shift(0), m[1][0], m[1][1], m[1][2], shift(1),
                     m[2][0], m[2][1], m[2][2], shift(2));
    return result;
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
TopoDS_Shape strip(gp_Pnt start, gp_Pnt end, gp_Vec offset) {
    BRepBuilderAPI_MakePolygon wire;
    wire.Add(start);
    wire.Add(end);
    wire.Add(end.Translated(offset));
    wire.Add(start.Translated(offset));
    wire.Close();
    require(wire.IsDone(), "Invalid bend strip");
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}
TopoDS_Shape bendSolid(const RecognizedBend &b, const TopTools_IndexedMapOfShape &faces,
                       double tolerance) {
    const auto f = face(faces, b.innerFace);
    BRepAdaptor_Surface s(f);
    require(s.GetType() == GeomAbs_Cylinder, "Only cylindrical bends are supported");
    double u0, u1, v0, v1;
    BRepTools::UVBounds(f, u0, u1, v0, v1);
    const auto cylinder = s.Cylinder();
    if (b.trimmedEnds) {
        const gp_Vec axis(cylinder.Axis().Direction());
        const auto at = [&](gp_Pnt p) { return gp_Vec(cylinder.Location(), p).Dot(axis); };
        const double first = at(point(b.axisStart)), last = at(point(b.axisEnd));
        const double minimum = std::min(first, last), maximum = std::max(first, last);
        BRepAdaptor_Surface outer(face(faces, b.outerFace));
        double a0, a1, z0, z1;
        BRepTools::UVBounds(face(faces, b.outerFace), a0, a1, z0, z1);
        const double outerFirst = at(outer.Value(a0, z0)), outerLast = at(outer.Value(a0, z1));
        require(v0 >= minimum - tolerance * 20 && v1 <= maximum + tolerance * 20 &&
                    std::min(outerFirst, outerLast) >= minimum - tolerance * 20 &&
                    std::max(outerFirst, outerLast) <= maximum + tolerance * 20,
                "A trimmed cylinder is outside its complete axial span");
        v0 = minimum;
        v1 = maximum;
    }
    require(std::abs((u1 - u0) - b.angleRadians) < 1e-6 &&
                std::abs((v1 - v0) - b.lengthMm) < tolerance * 20 &&
                (b.trimmedEnds || std::abs(area(f) - b.innerRadiusMm * (u1 - u0) * (v1 - v0)) <
                                      std::max(1e-5, area(f) * 1e-7)),
            "A bend has openings, cut edges or an incomplete cylindrical surface");
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
    gp_Trsf map; // Rigid world-to-flat placement of this wall's reference surface at Z=0.
    std::optional<std::size_t> incoming;
};
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
    const double minimumRail = bend.trimmedEnds ? bend.minimumStraightLengthMm : bend.lengthMm;
    require(properties.Mass() + std::max(tolerance * 20, bend.lengthMm * 1e-7) >= minimumRail,
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
const SheetLayout &OcctFlatPatternAccess::layout(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).layout;
}
const SheetOrientationVisual &OcctFlatPatternAccess::orientation(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).orientation;
}
static FlatPatternResult buildSheet(DocumentPtr document, FlatPatternRequest request,
                                    std::stop_token stop, bool geometryOnly) {
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
                    analysis.bends.size() <= maximumSheetBends,
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
            require(std::abs(b.thicknessMm - t) <= tolerance,
                    "Requires constant thickness and straight bends");
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
        if (std::abs(rootNormal.Dot(gp_Vec(axis))) > 1.e-7) {
            const auto &reference = bends[graph.at(root).front()];
            axis = gp_Dir(gp_Vec(point(reference.axisStart), point(reference.axisEnd)));
        }
        require(std::abs(rootNormal.Dot(gp_Vec(axis))) < 1.e-7,
                "Reference bend axis is outside its wall plane");
        placements.emplace(
            root, WallPlacement{placementMap(projected(gp_Pnt(0, 0, 0), rootPlane), axis,
                                             gp_Dir(toOther.Crossed(gp_Vec(axis))), gp_Pnt(0, 0, 0),
                                             gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)),
                                std::nullopt});
        std::vector<Wall> pending{root};
        std::vector<bool> used(bends.size());
        auto result = std::make_shared<CadFlatPattern>();
        result->source = document;
        result->data.settings = request.settings;
        result->data.thicknessMm = t;
        result->data.cutoutsIgnored = cutoutsIgnored;
        // Reserve wall slots in traversal order so links remain stable as children
        // are discovered. All solid/mapping fields are filled before publication.
        std::map<Wall, std::size_t> wallIndices{{root, 0}};
        TopoDS_Compound original;
        BRep_Builder builder;
        builder.MakeCompound(original);
        std::vector<TopoDS_Shape> patches;
        std::vector<FlatDisplayRegion> regions;
        std::vector<std::size_t> wallRegions;
        bool nonRadialEnds = false;
        bool partitionedSectors = false;
        for (std::size_t wi = 0; wi < pending.size(); ++wi) {
            cancelled(stop);
            const auto w = pending[wi];
            const auto &placement = placements.at(w);
            const auto pa = plane(faces, w[0]), pb = plane(faces, w[1]);
            require(pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-7) &&
                        std::abs(pa.Distance(pb.Location()) - t) <= tolerance,
                    "Inconsistent wall thickness");
            const gp_Vec n(pa.Axis().Direction());
            const auto wallSolid =
                extruded(face(faces, w[0]), n * gp_Vec(pa.Location(), pb.Location()).Dot(n));
            builder.Add(original, wallSolid);
            const auto mappedThickness =
                gp_Vec(pa.Location(), pb.Location()).Transformed(placement.map);
            const double zShift = mappedThickness.Z() < 0 ? t : 0;
            const auto wallMap = [placement, zShift](gp_Pnt p) {
                return p.Transformed(placement.map).Translated(gp_Vec(0, 0, zShift));
            };
            wallRegions.push_back(regions.size());
            regions.push_back({wallSolid, wallMap});
            gp_Trsf wallToFlat = placement.map;
            gp_Trsf thicknessShift;
            thicknessShift.SetTranslation(gp_Vec(0, 0, zShift));
            wallToFlat = thicknessShift * wallToFlat;
            if (result->layout.walls.size() <= wi)
                result->layout.walls.resize(wi + 1);
            result->layout.walls[wi] = {w, wallSolid, wallToFlat};
            patches.push_back(
                BRepBuilderAPI_Transform(face(faces, w[0]), placement.map, true).Shape());
            const auto flatBounds = bounds(patches.back());
            require(std::max(std::abs(flatBounds.minimum.z), std::abs(flatBounds.maximum.z)) <=
                        tolerance,
                    "A wall placement is outside the common flat plane");
            for (auto index : graph.at(w)) {
                if (placement.incoming == index)
                    continue;
                require(!used[index], "A sheet cycle cannot be unfolded without an explicit seam");
                const auto &b = bends[index];
                const auto child = wall(b, 0) == w ? wall(b, 1) : wall(b, 0);
                require(!placements.contains(child),
                        "A sheet cycle cannot be unfolded without an explicit seam");
                const gp_Dir bendAxis(gp_Vec(point(b.axisStart), point(b.axisEnd)));
                require(std::abs(pa.Axis().Direction().Dot(bendAxis)) < 1.e-7,
                        "A bend axis is outside its parent wall plane");
                gp_Dir outward(gp_Vec(bendAxis).Crossed(gp_Vec(pa.Axis().Direction())));
                if (materialSide(face(faces, w[0]), pa, b, outward, tolerance, stop) > 0)
                    outward.Reverse();
                const auto childPlane = plane(faces, child[0]);
                require(std::abs(childPlane.Axis().Direction().Dot(bendAxis)) < 1.e-7,
                        "A bend axis is outside its child wall plane");
                gp_Dir childAlong(gp_Vec(bendAxis).Crossed(gp_Vec(childPlane.Axis().Direction())));
                if (materialSide(face(faces, child[0]), childPlane, b, childAlong, tolerance,
                                 stop) < 0)
                    childAlong.Reverse();
                require(std::abs(std::acos(std::clamp(outward.Dot(childAlong), -1., 1.)) -
                                 b.angleRadians) < 1.e-6,
                        "Wall directions do not match the measured bend");
                const gp_Dir flatAxis(gp_Vec(bendAxis).Transformed(placement.map));
                const gp_Dir flatAcross(gp_Vec(outward).Transformed(placement.map));
                require(std::abs(flatAxis.Z()) < 1.e-7 && std::abs(flatAcross.Z()) < 1.e-7,
                        "A bend does not map into the flat plane");
                auto start = projected(point(b.axisStart), pa).Transformed(placement.map);
                auto end = projected(point(b.axisEnd), pa).Transformed(placement.map);
                start.SetZ(0);
                end.SetZ(0);
                const double k = *core::unfoldK(request.settings, b.innerRadiusMm, t);
                const auto ba = core::bendAllowance(b.innerRadiusMm, t, b.angleRadians, k);
                require(ba.has_value(), "Invalid bend parameters");
                const gp_Vec offset = gp_Vec(flatAcross) * *ba;
                placements.emplace(
                    child,
                    WallPlacement{placementMap(projected(point(b.axisStart), childPlane), bendAxis,
                                               childAlong, start.Translated(offset), flatAxis,
                                               flatAcross),
                                  index});
                pending.push_back(child);
                wallIndices.emplace(child, pending.size() - 1);
                used[index] = true;
                patches.push_back(strip(start, end, offset));
                const double turn =
                    gp_Vec(bendAxis).Dot(gp_Vec(outward).Crossed(gp_Vec(childAlong)));
                const double side = gp_Vec(flatAxis).Crossed(gp_Vec(flatAcross)).Z();
                result->data.bends.push_back({index,
                                              {start.X(), start.Y(), 0},
                                              {end.X(), end.Y(), 0},
                                              {offset.X(), offset.Y(), 0},
                                              b.angleRadians,
                                              turn * side >= 0 ? 1 : -1,
                                              b.innerRadiusMm,
                                              k,
                                              *ba});
                const auto fullSector = bendSolid(b, faces, tolerance);
                auto sector = b.trimmedEnds
                                  ? boolean<BRepAlgoAPI_Common>(fullSector, source, tolerance, stop)
                                  : fullSector;
                if (b.trimmedEnds) {
                    TopTools_IndexedMapOfShape components;
                    TopExp::MapShapes(sector, TopAbs_SOLID, components);
                    if (components.Extent() > 1) {
                        const auto owned = ownedBendSector(
                            source, fullSector,
                            {face(faces, b.innerFace), face(faces, b.outerFace)}, tolerance, stop);
                        cancelled(stop);
                        require(owned.has_value(),
                                "Cannot assign both complete bend skins to one sector");
                        sector = *owned;
                        partitionedSectors = true;
                    }
                    bool nonRadial = false;
                    const auto radial = mapTrimmedBend(
                        sector, [](gp_Pnt p) { return p; }, 1, tolerance, nullptr, stop,
                        &nonRadial);
                    nonRadialEnds |= nonRadial;
                    const double budget = std::max(1e-4, volume(source) * 1e-7);
                    const double missing =
                        volume(boolean<BRepAlgoAPI_Cut>(sector, radial, tolerance, stop));
                    const double excess =
                        volume(boolean<BRepAlgoAPI_Cut>(radial, sector, tolerance, stop));
                    if (missing > budget || excess > budget)
                        throw FlatPatternError{FlatPatternErrorCode::UnsupportedGeometry,
                                               "The trimmed end is not a supported radial ruled "
                                               "surface: missing=" +
                                                   std::to_string(missing) +
                                                   " excess=" + std::to_string(excess)};
                }
                builder.Add(original, sector);
                const auto center = point(b.axisStart);
                const gp_Dir radial(gp_Vec(center, projected(center, pa)));
                const gp_Dir otherRadial(gp_Vec(center, projected(center, childPlane)));
                const double sign =
                    gp_Vec(bendAxis).Dot(gp_Vec(radial).Crossed(gp_Vec(otherRadial))) >= 0 ? 1 : -1;
                const double zInner =
                    wallMap(center.Translated(gp_Vec(radial) * b.innerRadiusMm)).Z();
                const double zSign = gp_Vec(radial).Transformed(placement.map).Z();
                regions.push_back(
                    {sector, [center, bendAxis, radial, sign, start, flatAxis, offset, zInner,
                              zSign, b](gp_Pnt p) {
                         auto r = gp_Vec(center, p);
                         const double axial = r.Dot(gp_Vec(bendAxis));
                         r -= gp_Vec(bendAxis) * axial;
                         const double angle = std::clamp(
                             std::atan2(sign * gp_Vec(bendAxis).Dot(gp_Vec(radial).Crossed(r)),
                                        gp_Vec(radial).Dot(r)),
                             0., b.angleRadians);
                         auto mapped = start.Translated(gp_Vec(flatAxis) * axial +
                                                        offset * (angle / b.angleRadians));
                         mapped.SetZ(zInner + zSign * (r.Magnitude() - b.innerRadiusMm));
                         return mapped;
                     }});
                TopoDS_Shape flatSector;
                if (b.trimmedEnds) {
                    TopoDS_Face patch;
                    flatSector =
                        mapTrimmedBend(sector, regions.back().map, 0, tolerance, &patch, stop);
                    gp_Trsf lower;
                    lower.SetTranslation(gp_Vec(0, 0, -zInner));
                    patches.back() = BRepBuilderAPI_Transform(patch, lower, true).Shape();
                } else
                    flatSector = extruded(TopoDS::Face(patches.back()), gp_Vec(0, 0, t));
                const auto tangent = center.Translated(gp_Vec(radial) * b.innerRadiusMm);
                const gp_Vec bx = gp_Vec(bendAxis).Crossed(gp_Vec(radial)) * sign;
                const gp_Vec by = -gp_Vec(radial), bz(bendAxis);
                const simulation::SingleBend model{
                    t, b.innerRadiusMm, b.angleRadians, b.lengthMm, 1, 1};
                const auto atProgress = [tangent, bx, by, bz, model](gp_Pnt p, double progress) {
                    const gp_Vec v(tangent, p);
                    const auto section = simulation::singleBendSection(model, progress);
                    if (!section)
                        throw std::runtime_error("Invalid draft bend progress");
                    const auto mapped =
                        simulation::mapBendPoint(model, {v.Dot(bx), v.Dot(by)}, *section);
                    if (!mapped)
                        throw std::runtime_error("Cannot map draft bend surface");
                    return tangent.Translated(bx * mapped->x + by * mapped->y + bz * v.Dot(bz));
                };
                const auto childTangent = center.Translated(gp_Vec(otherRadial) * b.innerRadiusMm);
                const auto childAtProgress = [atProgress, childTangent, center, bendAxis, sign,
                                              angle = b.angleRadians](double progress) {
                    gp_Trsf rotation, translation;
                    rotation.SetRotation(gp_Ax1(center, bendAxis), sign * (progress - 1) * angle);
                    translation.SetTranslation(gp_Vec(childTangent.Transformed(rotation),
                                                      atProgress(childTangent, progress)));
                    return translation * rotation;
                };
                const gp_Ax3 sectionAxes(sign > 0 ? tangent : tangent.Translated(bz * b.lengthMm),
                                         gp_Dir(bx.Crossed(by)), gp_Dir(bx));
                const auto solidAtProgress = [model, sectionAxes, sector, atProgress, b,
                                              tolerance](double progress) {
                    if (b.trimmedEnds)
                        return mapTrimmedBend(
                            sector,
                            [atProgress, progress](gp_Pnt p) { return atProgress(p, progress); },
                            progress, tolerance);
                    return makeRouteSector(model, sectionAxes, progress);
                };
                result->layout.bends.push_back({index, wallIndices.at(w), wallIndices.at(child),
                                                sector, flatSector, regions.back().map, atProgress,
                                                childAtProgress, solidAtProgress, sectionAxes});
            }
        }
        require(placements.size() == graph.size() &&
                    std::all_of(used.begin(), used.end(), [](bool b) { return b; }),
                "Incomplete or disconnected sheet graph");
        std::sort(result->data.bends.begin(), result->data.bends.end(),
                  [](const auto &a, const auto &b) { return a.bendIndex < b.bendIndex; });
        const double budget = std::max(1e-4, volume(source) * 1e-7);
        if (partitionedSectors) {
            // An envelope can also contain a neighbour's material. Every
            // chosen sector owns full real skins; sectors may not own the
            // same volume. Walls are proved separately below.
            std::vector<Bnd_Box> sectorBoxes(result->layout.bends.size());
            for (std::size_t i = 0; i < sectorBoxes.size(); ++i) {
                BRepBndLib::AddOptimal(result->layout.bends[i].solid, sectorBoxes[i], false, false);
                require(!sectorBoxes[i].IsVoid() && !sectorBoxes[i].IsOpen(),
                        "Invalid bend sector bounds");
                sectorBoxes[i].Enlarge(tolerance);
            }
            for (std::size_t i = 0; i < result->layout.bends.size(); ++i)
                for (std::size_t j = 0; j < i; ++j) {
                    cancelled(stop);
                    if (sectorBoxes[i].IsOut(sectorBoxes[j]))
                        continue;
                    const auto overlap = boolean<BRepAlgoAPI_Common>(
                        result->layout.bends[i].solid, result->layout.bends[j].solid, tolerance,
                        stop, "Bend sector ownership intersection failed");
                    require(volume(overlap) <= budget, "Bend sectors share material");
                }
        }
        TopoDS_Shape reconstruction = original;
        const auto reconstructs = [&](bool allowEmptyDifference = false) {
            const double actualVolume = volume(reconstruction), sourceVolume = volume(source);
            if (!std::isfinite(actualVolume) || !std::isfinite(sourceVolume) || actualVolume <= 0 ||
                sourceVolume <= 0)
                return false;
            const bool equalVolume = std::abs(actualVolume - sourceVolume) <= budget;
            if (!equalVolume && !allowEmptyDifference)
                return false;
            const auto missing = boolean<BRepAlgoAPI_Cut>(source, reconstruction, tolerance, stop,
                                                          "Source coverage cut failed");
            if (equalVolume && volume(missing) > budget)
                return false;
            const auto excess = boolean<BRepAlgoAPI_Cut>(reconstruction, source, tolerance, stop,
                                                         "Reverse source coverage cut failed");
            if (equalVolume)
                return volume(excess) <= budget;
            // A tolerance-bearing BRep can integrate differently after an
            // exact partition. Only EMPTY topology in BOTH directions may
            // resolve that numerical mismatch; no larger volume budget.
            const auto empty = [](const TopoDS_Shape &shape) {
                return shape.ShapeType() == TopAbs_COMPOUND && !TopoDS_Iterator(shape).More();
            };
            return empty(missing) && empty(excess);
        };
        bool complete = reconstructs();
        if ((!complete && nonRadialEnds) || partitionedSectors) {
            // An oblique cut may also bevel a rigid wall through its thickness.
            // Replace failed prism approximations by complete, owned source
            // components. Both original-solid differences still follow.
            std::vector<TopoDS_Shape> sectors;
            for (const auto &region : result->layout.bends)
                sectors.push_back(region.solid);
            std::vector<std::array<TopoDS_Face, 2>> anchors;
            for (const auto &region : result->layout.walls)
                anchors.push_back({face(faces, region.faces[0]), face(faces, region.faces[1])});
            const auto actual = isolateFlatWalls(source, sectors, anchors, tolerance, stop);
            cancelled(stop);
            require(actual && actual->size() == result->layout.walls.size(),
                    "Cannot isolate all complete planar walls after removing bend sectors");
            TopoDS_Compound remainder;
            builder.MakeCompound(remainder);
            for (std::size_t i = 0; i < actual->size(); ++i) {
                result->layout.walls[i].solid = (*actual)[i];
                regions[wallRegions[i]].solid = (*actual)[i];
                if (i > 0)
                    builder.Add(remainder, (*actual)[i]);
            }
            for (const auto &sector : sectors)
                builder.Add(remainder, sector);
            // Source tolerances can leave coincident boundary fragments in
            // independent pieces. Compare their real union with the source;
            // never add the same material twice via compound volume sums.
            reconstruction = boolean<BRepAlgoAPI_Fuse>((*actual)[0], remainder, tolerance, stop,
                                                       "Wall/bend reconstruction union failed");
            complete = reconstructs(true);
        }
        require(complete,
                "The recognized walls and bends do not reconstruct the complete original solid");
        if (geometryOnly) {
            cancelled(stop);
            return FlatPatternPtr(std::move(result));
        }
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
        unify.SetLinearTolerance(tolerance);
        unify.SetAngularTolerance(1.e-7);
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
            b.start.x -= box.minimum.x;
            b.end.x -= box.minimum.x;
            b.start.y -= box.minimum.y;
            b.end.y -= box.minimum.y;
        }
        result->display =
            extruded(TopoDS::Face(TopExp_Explorer(flat, TopAbs_FACE).Current()), gp_Vec(0, 0, t));
        result->geometry = result->display;
        result->data.bounds = bounds(result->display);
        const auto &orientation = OcctDocumentAccess::sheetOrientation(*native);
        std::optional<SheetReference> flatReference;
        if (orientation.available) {
            std::vector<TopoDS_Shape> walls;
            for (const auto &wall : result->layout.walls)
                walls.push_back(wall.solid);
            if (const auto at = sheetReferenceRegion(orientation.reference, walls, tolerance)) {
                flatReference = movedSheetReference(orientation.reference,
                                                    origin * result->layout.walls[*at].toFlat);
                flatReference->thicknessMm = t;
                result->orientation.edges = flatReference->edges;
                if (std::abs(flatReference->normal.Z()) > 1 - 1.e-7)
                    result->data.positiveZIsSideA = flatReference->normal.Z() > 0;
            }
        }
        if (cutoutsIgnored) {
            auto cutouts = flatCutoutDisplay(OcctDocumentAccess::shape(*native), regions,
                                             gp_Vec(-box.minimum.x, -box.minimum.y, 0), tolerance,
                                             stop, &orientation);
            result->display = std::move(cutouts.surface);
            result->contours = std::move(cutouts.contours);
            appendSheetSides(result->orientation, result->display, cutouts.faceSides);
        } else {
            BRepMesh_IncrementalMesh mesh(result->display, .08, false, .25, false);
            require(mesh.IsDone(), "Flat preview meshing failed");
            if (flatReference) {
                auto flatMetadata = native->metadata();
                flatMetadata.bendAnalysis = {};
                const auto tags =
                    classifySheetOrientation(result->display, flatMetadata, stop, flatReference);
                result->orientation = sheetOrientationVisual(result->display, tags);
            }
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
            const auto ends = b.centreLine();
            auto line = BRepBuilderAPI_MakeEdge(gp_Pnt(ends[0].x, ends[0].y, t + .005),
                                                gp_Pnt(ends[1].x, ends[1].y, t + .005))
                            .Shape();
            result->lines.push_back(line);
            builder.Add(display, line);
        }
        result->display = display;
        try {
            if (const auto measures = OcctDocumentAccess::measurements(*native))
                result->data.measurements = flatModelMeasurements(
                    *measures, OcctDocumentAccess::shape(*native), source, flat,
                    result->layout.walls, origin, cutoutsIgnored, tolerance, stop);
        } catch (const Standard_Failure &) {
            result->data.measurements
                .reset(); // Do not block a valid unfolding/PDF for optional annotations.
        } catch (const std::exception &) {
            result->data.measurements.reset();
        }
        cancelled(stop);
        return FlatPatternPtr(std::move(result));
    } catch (const TrimmedBendError &e) {
        return FlatPatternError{stop.stop_requested() ? FlatPatternErrorCode::Cancelled
                                                      : FlatPatternErrorCode::UnsupportedGeometry,
                                e.what()};
    } catch (const FlatPatternError &e) {
        return e;
    } catch (const Standard_Failure &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed,
                                e.GetMessageString() ? e.GetMessageString() : "CAD failure"};
    } catch (const std::exception &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed, e.what()};
    }
}
FlatPatternResult FlatPatternBuilder::build(DocumentPtr document, FlatPatternRequest request,
                                            std::stop_token stop) const {
    return buildSheet(std::move(document), request, stop, false);
}
SheetGeometryResult prepareSheetGeometry(DocumentPtr document, std::stop_token stop) {
    const auto result =
        buildSheet(document, {{core::SheetMaterial::Unknown, .5, 1}, {}}, stop, true);
    if (const auto *error = std::get_if<FlatPatternError>(&result))
        return *error;
    const auto &owner = std::get<FlatPatternPtr>(result);
    const auto &layout = dynamic_cast<const CadFlatPattern &>(*owner).layout;
    return VerifiedSheetGeometry{std::move(document),
                                 std::shared_ptr<const SheetLayout>(owner, &layout)};
}
} // namespace bending::cad
