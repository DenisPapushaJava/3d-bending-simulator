#include "BendFixtures.h"
#include "FlatCutProfileBuilder.h"
#include "TrayFixtures.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Line.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
using namespace bending;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
application::DocumentPtr load(const TopoDS_Shape &shape, const std::filesystem::path &path) {
    STEPControl_Writer w;
    check(w.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone, "Transfer failed");
    check(w.Write(path.string().c_str()) == IFSelect_RetDone, "Write failed");
    auto d = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    check(d != nullptr, "Import failed");
    return d;
}
application::FlatPatternPtr build(application::DocumentPtr d,
                                  core::UnfoldSettings s = {
                                      core::SheetMaterial::MildSteel, {}, 1}) {
    auto result = cad::FlatPatternBuilder{}.build(d, {s, {}}, {});
    if (const auto *e = std::get_if<application::FlatPatternError>(&result))
        throw std::runtime_error(e->detail);
    return std::get<application::FlatPatternPtr>(result);
}
double displayVolume(const application::FlatPattern &flat) {
    double volume = 0;
    for (TopExp_Explorer e(cad::OcctFlatPatternAccess::shape(flat), TopAbs_FACE); e.More();
         e.Next()) {
        const auto face = TopoDS::Face(e.Current());
        TopLoc_Location location;
        const auto mesh = BRep_Tool::Triangulation(face, location);
        check(!mesh.IsNull(), "Unmeshed flat display face");
        for (int i = 1; i <= mesh->NbTriangles(); ++i) {
            int a, b, c;
            mesh->Triangle(i).Get(a, b, c);
            const auto p = mesh->Node(a).Transformed(location.Transformation()).XYZ();
            const auto q = mesh->Node(b).Transformed(location.Transformation()).XYZ();
            const auto r = mesh->Node(c).Transformed(location.Transformation()).XYZ();
            volume += p.Dot(q.Crossed(r)) / 6 * (face.Orientation() == TopAbs_REVERSED ? -1 : 1);
        }
    }
    return std::abs(volume);
}
TopoDS_Shape reverseProfile(double axialLength = 80, double height = 40, double leftEnd = -30,
                            double rightEnd = 40) {
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    const auto arc = [&](double x, double y, double r, double from, double to) {
        wire.Add(
            BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(fixtures::polar(r, from, x, y),
                                                       fixtures::polar(r, (from + to) / 2, x, y),
                                                       fixtures::polar(r, to, x, y))
                                        .Value())
                .Edge());
    };
    constexpr double pi = std::numbers::pi;
    line({leftEnd, 0, 0}, {0, 0, 0});
    arc(0, 6, 6, -pi / 2, 0);
    line({6, 6, 0}, {6, height - 6, 0});
    arc(10, height - 6, 4, pi, pi / 2);
    line({10, height - 2, 0}, {rightEnd, height - 2, 0});
    line({rightEnd, height - 2, 0}, {rightEnd, height, 0});
    line({rightEnd, height, 0}, {10, height, 0});
    arc(10, height - 6, 6, pi / 2, pi);
    line({4, height - 6, 0}, {4, 6, 0});
    arc(0, 6, 4, 0, -pi / 2);
    line({0, 2, 0}, {leftEnd, 2, 0});
    line({leftEnd, 2, 0}, {leftEnd, 0, 0});
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, axialLength))
        .Shape();
}

TopoDS_Shape segmentedWall(bool holes = true) {
    const auto moved = [](const TopoDS_Shape &s, double z) {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(0, 0, z));
        return BRepBuilderAPI_Transform(s, t, true).Shape();
    };
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0)));
    auto shape =
        BRepAlgoAPI_Fuse(
            reverseProfile(98),
            moved(BRepBuilderAPI_Transform(reverseProfile(100), mirror, true).Shape(), 100))
            .Shape();
    shape = BRepAlgoAPI_Fuse(shape, moved(reverseProfile(98), 202)).Shape();
    shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(4, 6, 0), 2, 28, 300)).Shape();
    if (holes) {
        for (double z : {25., 75., 125., 175., 225., 275.})
            for (double y : {12., 28.})
                shape =
                    BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(
                                               gp_Ax2(gp_Pnt(3, y, z), gp_Dir(1, 0, 0)), 1.5, 4))
                        .Shape();
        for (const auto p : {gp_Pnt(-15, -1, 49), gp_Pnt(25, -1, 150), gp_Pnt(-15, -1, 251)})
            shape =
                BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(gp_Ax2(p, gp_Dir(0, 1, 0)), 3, 4))
                    .Shape();
    }
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
    unify.Build();
    return unify.Shape();
}
TopoDS_Shape overlappingTabs() {
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(10, 0, 0), gp_Dir(1, 0, 0)));
    auto shape =
        BRepAlgoAPI_Fuse(fixtures::bentStrip(),
                         BRepBuilderAPI_Transform(fixtures::bentStrip(), mirror, true).Shape())
            .Shape();
    shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(-30, 4, -12), 30, 2, 12)).Shape();
    shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(20, 4, -12), 30, 2, 12)).Shape();
    shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(-30, 4, -12), 80, 2, 10)).Shape();
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
    unify.Build();
    return unify.Shape();
}
TopoDS_Shape closedTube() {
    const auto loop = [](double radius) {
        BRepBuilderAPI_MakeWire wire;
        const auto line = [&](gp_Pnt a, gp_Pnt b) {
            wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
        };
        const auto arc = [&](double x, double y, double start, double end) {
            wire.Add(BRepBuilderAPI_MakeEdge(
                         GC_MakeArcOfCircle(fixtures::polar(radius, start, x, y),
                                            fixtures::polar(radius, (start + end) / 2, x, y),
                                            fixtures::polar(radius, end, x, y))
                             .Value())
                         .Edge());
        };
        const double pi = std::numbers::pi;
        line({6, 6 - radius, 0}, {74, 6 - radius, 0});
        arc(74, 6, -pi / 2, 0);
        line({74 + radius, 6, 0}, {74 + radius, 54, 0});
        arc(74, 54, 0, pi / 2);
        line({74, 54 + radius, 0}, {6, 54 + radius, 0});
        arc(6, 54, pi / 2, pi);
        line({6 - radius, 54, 0}, {6 - radius, 6, 0});
        arc(6, 6, pi, pi * 1.5);
        return wire.Wire();
    };
    BRepBuilderAPI_MakeFace face(loop(6));
    face.Add(TopoDS::Wire(loop(4).Reversed()));
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, 0, 80)).Shape();
}
void treePatterns(const std::filesystem::path &folder) {
    const auto document = load(segmentedWall(), folder / "segmented.step");
    check(document->metadata().bendAnalysis.bends.size() == 6,
          "Segmented fixture must contain six complete bends");
    const auto source = std::string(document->sourceStep());
    const auto flat = build(document);
    const auto &info = flat->info();
    const double ba = std::numbers::pi / 2 * (4 + .43 * 2);
    check(info.bends.size() == 6 && std::abs(info.bounds.maximum.x - 300) < 1e-4 &&
              std::abs(info.bounds.maximum.y - (28 + 60 + 2 * ba)) < 1e-4,
          "Wrong branching blank dimensions");
    check(std::count_if(info.bends.begin(), info.bends.end(),
                        [](const auto &b) { return b.direction > 0; }) == 3,
          "Mirrored tab directions are not retained");
    for (const auto &b : info.bends) {
        check(std::abs(b.allowanceMm - ba) < 1e-5, "Branch allowance mismatch");
        check(std::abs(b.lengthMm() -
                       document->metadata().bendAnalysis.bends[b.bendIndex].lengthMm) < 1e-4,
              "Branch length changed");
    }
    GProp_GProps properties;
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*flat), properties);
    const double expectedArea = 300 * 28 + 2 * 296 * 30 + 2 * 296 * ba -
                                12 * std::numbers::pi * 1.5 * 1.5 - 3 * std::numbers::pi * 9;
    check(
        info.cutoutsIgnored &&
            std::abs(std::abs(properties.Mass()) -
                     (expectedArea + 12 * std::numbers::pi * 1.5 * 1.5 + 3 * std::numbers::pi * 9) *
                         2) < 1e-3 &&
            std::abs(displayVolume(*flat) - expectedArea * 2) < 2,
        "Slots or holes were lost in the branched contour");
    const auto solid =
        TopExp_Explorer(cad::OcctFlatPatternAccess::geometry(*flat), TopAbs_SOLID).Current();
    for (double x : {99., 201.}) {
        check(BRepClass3d_SolidClassifier(solid, gp_Pnt(x, 5, 1), 1e-5).State() == TopAbs_OUT &&
                  BRepClass3d_SolidClassifier(solid, gp_Pnt(x, info.bounds.maximum.y - 5, 1), 1e-5)
                          .State() == TopAbs_OUT &&
                  BRepClass3d_SolidClassifier(solid, gp_Pnt(x, info.bounds.maximum.y / 2, 1), 1e-5)
                          .State() == TopAbs_IN,
              "A branch gap was filled or the continuous wall was cut");
    }
    check(document->sourceStep() == source && document->metadata().bendAnalysis.bends.size() == 6,
          "Source was mutated");
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
    auto rotated = build(load(BRepBuilderAPI_Transform(segmentedWall(), rotation, true).Shape(),
                              folder / "segmented-rotated.step"));
    check(std::abs(rotated->info().bounds.maximum.x - info.bounds.maximum.x) < 1e-4 &&
              std::abs(rotated->info().bounds.maximum.y - info.bounds.maximum.y) < 1e-4,
          "Branch layout depends on STEP world axes");
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    const auto mirrored =
        build(load(BRepBuilderAPI_Transform(segmentedWall(), mirror, true).Shape(),
                   folder / "segmented-mirrored.step"));
    check(std::abs(mirrored->info().bounds.maximum.x - info.bounds.maximum.x) < 1e-4 &&
              std::abs(mirrored->info().bounds.maximum.y - info.bounds.maximum.y) < 1e-4 &&
              std::abs(displayVolume(*mirrored) - expectedArea * 2) < 2 &&
              std::count_if(mirrored->info().bends.begin(), mirrored->info().bends.end(),
                            [](const auto &b) { return b.direction > 0; }) == 3,
          "Mirroring lost branch dimensions, holes or reverse bends");
    auto overlap = load(overlappingTabs(), folder / "overlap.step");
    check(overlap->metadata().bendAnalysis.bends.size() == 2,
          "Overlap fixture did not recognize two bends");
    auto result =
        cad::FlatPatternBuilder{}.build(overlap, {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
    check(std::holds_alternative<application::FlatPatternError>(result) &&
              std::get<application::FlatPatternError>(result).detail == "Flat wall patches overlap",
          "Overlapping unfolded tabs were accepted or not checked");
    const auto tube = load(closedTube(), folder / "closed.step");
    check(tube->metadata().bendAnalysis.bends.size() == 4, "Closed fixture must have four bends");
    result =
        cad::FlatPatternBuilder{}.build(tube, {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
    check(std::holds_alternative<application::FlatPatternError>(result) &&
              std::get<application::FlatPatternError>(result).detail ==
                  "Multiple bend paths disagree on the wall placement",
          "Closed sheet was split without a seam");
}

void cyclicReliefPattern(const std::filesystem::path &folder) {
    // Cutting out the web and both bends in the middle leaves two separate
    // paths between the same continuous flanges. No seam is needed.
    const auto relief = BRepPrimAPI_MakeBox(gp_Pnt(-2, -1, 30), 14, 42, 20).Shape();
    const auto document = load(BRepAlgoAPI_Cut(reverseProfile(), relief).Shape(),
                               folder / "relief-across-two-bends.step");
    check(document->metadata().bendAnalysis.bends.size() == 4,
          "Cycle fixture did not retain all four bend zones");
    const auto flat = build(document, {core::SheetMaterial::MildSteel, .5, 1});
    const auto &info = flat->info();
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    const double allowance = std::numbers::pi / 2 * 5;
    const double developedWidth = 28 + 60 + 2 * allowance;
    check(layout.walls.size() == 4 && layout.bends.size() == 4 && info.bends.size() == 4 &&
              std::abs(info.bounds.maximum.x - 80) < 1.e-5 &&
              std::abs(info.bounds.maximum.y - developedWidth) < 1.e-5 && info.cutProfile &&
              info.cutProfile->loops.size() == 2,
          "A consistent bend cycle lost a wall, bend or internal relief");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*flat), properties);
    check(std::abs(std::abs(properties.Mass()) -
                   2 * (80 * developedWidth - 20 * (28 + 4 + 2 * allowance))) < 1.e-4,
          "Unfolding the cycle added or removed material");
}

void coplanarWallPattern(const std::filesystem::path &folder) {
    const auto profile = reverseProfile();
    const auto lower = BRepPrimAPI_MakeBox(gp_Pnt(-50, -10, -5), 100, 30, 90).Shape();
    const auto upper = BRepPrimAPI_MakeBox(gp_Pnt(-50, 20, -5), 100, 30, 90).Shape();
    const auto joined = BRepAlgoAPI_Fuse(BRepAlgoAPI_Common(profile, lower).Shape(),
                                         BRepAlgoAPI_Common(profile, upper).Shape())
                            .Shape();
    const auto drill =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(3, 20, 40), gp_Dir(1, 0, 0)), 3, 4).Shape();
    const auto document =
        load(BRepAlgoAPI_Cut(joined, drill).Shape(), folder / "coplanar-wall-seam.step");
    const auto reference = build(
        load(BRepAlgoAPI_Cut(profile, drill).Shape(), folder / "coplanar-wall-reference.step"));
    const auto flat = build(document);
    const auto &info = flat->info();
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    check(layout.usesJoinedWallFaces && layout.walls.size() == 3 && info.bends.size() == 2 &&
              info.cutProfile && info.cutProfile->loops.size() == 2 &&
              std::abs(info.cutProfile->loops.back().radiusMm - 3) < 1.e-6 &&
              std::abs(info.bounds.maximum.x - reference->info().bounds.maximum.x) < 1.e-5 &&
              std::abs(info.bounds.maximum.y - reference->info().bounds.maximum.y) < 1.e-5,
          "Joining coplanar wall faces lost the through hole or changed the developed stock");
}

void splitLinkPattern(const std::filesystem::path &folder) {
    // The relief interrupts both skins of the bend but leaves both flanges
    // continuous. In the flat stock it is one internal rectangular opening.
    const auto blank = fixtures::bentStrip();
    const auto relief = BRepPrimAPI_MakeBox(gp_Pnt(-3, -2, 30), 12, 11, 20).Shape();
    const auto doc = load(BRepAlgoAPI_Cut(blank, relief).Shape(), folder / "split-link.step");
    check(doc->metadata().bendAnalysis.bends.size() == 2,
          "Relief fixture must contain two complete bend zones");
    const auto flat = build(doc, {core::SheetMaterial::MildSteel, .5, 1});
    const auto &info = flat->info();
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    const auto allowance = std::numbers::pi / 2 * 5;
    check(layout.walls.size() == 2 && layout.bends.size() == 2 && info.bends.size() == 2 &&
              std::abs(info.bounds.maximum.x - 80) < 1.e-5 &&
              std::abs(info.bounds.maximum.y - (60 + allowance)) < 1.e-5,
          "Split bend did not preserve the two flanges and the two bend zones");
    for (const auto &bend : info.bends)
        check(std::abs(bend.lengthMm() - 30) < 1.e-5 &&
                  std::abs(bend.allowanceMm - allowance) < 1.e-5,
              "Split bend merged the relief interval into a continuous bend");
    check(info.cutProfile && info.cutProfile->loops.size() == 2,
          "DXF cutting profile lost the internal relief");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*flat), properties);
    check(std::abs(std::abs(properties.Mass()) -
                   2 * (80 * (60 + allowance) - 20 * (5 + allowance))) < 1.e-4,
          "Flat geometry added or removed material at the relief");

    application::RouteDraftRequest draft;
    application::BendRouteRequest route;
    for (const auto &bend : info.bends) {
        draft.operations.push_back({"B" + std::to_string(bend.bendIndex + 1),
                                    bend.bendIndex,
                                    {bend.turnRadians, bend.radiusMm},
                                    {},
                                    {}});
    }
    route.operations = draft.operations;
    cad::SingleBendPreviewBuilder planner;
    const auto blankResult = planner.buildRouteDraft(doc, draft, {});
    check(std::holds_alternative<application::RouteDraftPtr>(blankResult),
          "Shared bend has no flat draft pose");
    draft.step = 1;
    const auto draftResult = planner.buildRouteDraft(doc, draft, {});
    if (const auto *error = std::get_if<application::BendRouteError>(&draftResult))
        throw std::runtime_error("Shared bend draft: " + error->detail);
    const auto draftMotion = std::get<application::RouteDraftPtr>(draftResult);
    check(draftMotion->visual->unverifiedDraft() && draftMotion->visual->activeBendCount() == 2 &&
              draftMotion->visual->frameCount() == 21 &&
              std::abs(draftMotion->visual->model().widthMm - 80) < 1.e-5,
          "Shared draft did not use one motion over the complete axial span");
    const auto repeated = planner.buildRouteDraft(doc, draft, {});
    check(std::get<application::RouteDraftPtr>(repeated)->reusedVisual,
          "Shared draft did not reuse its checked positions");
    draft.step = 2;
    check(std::get<application::BendRouteError>(planner.buildRouteDraft(doc, draft, {})).code ==
              application::BendRouteErrorCode::InvalidInput,
          "A shared bend exposed a second independent draft step");

    const auto routeResult = planner.buildRoute(doc, route, {});
    if (const auto *error = std::get_if<application::BendRouteError>(&routeResult))
        throw std::runtime_error("Shared bend route: " + error->detail);
    const auto checked = std::get<application::BendRoutePtr>(routeResult);
    check(checked->steps.size() == 1 && checked->steps.front().additionalBends.size() == 1 &&
              checked->steps.front().preview->activeBendCount() == 2 &&
              checked->statistics.builtTransitions == 1,
          "Shared bend was evaluated as more than one physical stroke");
    route.withTools = true;
    for (auto &op : route.operations)
        op.tools = core::PressTools{16, std::numbers::pi / 3, std::numbers::pi / 6, .8, 65, 40};
    const auto tooShort = planner.buildRoute(doc, route, {});
    check(std::get<application::BendRouteError>(tooShort).operationError ==
              application::BendPreviewErrorCode::ToolTooShort,
          "A tool shorter than the joint span was accepted");

    // An oblique end of the same internal relief maps to a spline, even
    // though the external perimeter is still a rectangle.
    const auto inclinedPlane =
        BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 46), gp_Dir(-.6, 0, 1))).Face();
    const auto above = BRepPrimAPI_MakeHalfSpace(inclinedPlane, gp_Pnt(0, 0, 60)).Solid();
    const auto obliqueRelief = BRepAlgoAPI_Cut(relief, above).Shape();
    const auto obliqueDoc =
        load(BRepAlgoAPI_Cut(blank, obliqueRelief).Shape(), folder / "split-link-oblique.step");
    const auto oblique = build(obliqueDoc, {core::SheetMaterial::MildSteel, .5, 1});
    const auto &cut = oblique->info().cutProfile;
    check(cut && cut->midThickness && cut->loops.size() == 2 && cut->loops[1].vertices.size() > 4 &&
              oblique->info().bends.size() == 2,
          "Curved internal relief was lost or rejected in the cutting profile");
}

void manySegmentsOfOneBend(const std::filesystem::path &folder) {
    constexpr int slotCount = 32;
    constexpr double slotLength = 1;
    BRep_Builder builder;
    TopoDS_Compound reliefs;
    builder.MakeCompound(reliefs);
    for (int i = 0; i < slotCount; ++i) {
        const auto slot =
            BRepPrimAPI_MakeBox(gp_Pnt(-3, -2, 2 + 2.4 * i), 12, 11, slotLength).Shape();
        builder.Add(reliefs, slot);
    }
    BRepAlgoAPI_Cut cut(fixtures::bentStrip(), reliefs);
    check(cut.IsDone() && !cut.HasErrors(), "Split bend fixture cut failed");
    const auto document = load(cut.Shape(), folder / "many-segments.step");
    const auto flat = build(document, {core::SheetMaterial::MildSteel, .5, 1});
    const auto &info = flat->info();
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    check(layout.walls.size() == 2 && info.bends.size() == slotCount + 1 && info.cutProfile &&
              info.cutProfile->loops.size() == slotCount + 1,
          "Unfolding lost separate bend sectors or reliefs beyond the route limit");
    double length = 0;
    for (const auto &bend : info.bends)
        length += bend.lengthMm();
    check(std::abs(length - (80 - slotCount * slotLength)) < 1.e-5,
          "Bend sectors fill a relief or lose material");
}

void linearCutJoin() {
    const auto buildContour = [](double gap, double tolerance) {
        const std::array<gp_Pnt, 4> points = {gp_Pnt(0, 0, 0), gp_Pnt(30, 0, 0), gp_Pnt(30, 20, 0),
                                              gp_Pnt(0, 20, 0)};
        std::array<TopoDS_Vertex, 4> vertices;
        for (std::size_t i = 0; i < vertices.size(); ++i)
            vertices[i] = BRepBuilderAPI_MakeVertex(points[i]);
        BRep_Builder{}.UpdateVertex(vertices[1], .01);
        const Handle(Geom_Line) side = new Geom_Line(gp_Pnt(30, gap, 0), gp_Dir(0, 1, 0));
        const auto edge =
            BRepBuilderAPI_MakeEdge(side, vertices[1], vertices[2], 0, 20 - gap).Edge();
        BRepBuilderAPI_MakeWire wire;
        wire.Add(BRepBuilderAPI_MakeEdge(vertices[0], vertices[1]));
        wire.Add(edge);
        wire.Add(BRepBuilderAPI_MakeEdge(vertices[2], vertices[3]));
        wire.Add(BRepBuilderAPI_MakeEdge(vertices[3], vertices[0]));
        const auto face =
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(), gp_Dir(0, 0, 1)), wire.Wire()).Face();
        check(BRepCheck_Analyzer(face).IsValid() &&
                  std::abs(BRepAdaptor_Curve(edge).Value(0).Y() - gap) < 1.e-9,
              "DXF join fixture did not preserve the topological endpoint and curve gap");
        return cad::buildFlatCutProfile(face, {}, {}, {}, {}, {}, 1, tolerance, {});
    };
    const auto cut = buildContour(.000667, .01);
    check(cut && cut->loops.size() == 1 && cut->loops.front().vertices.size() == 4,
          "A bounded shared-vertex line join did not preserve the four cutting segments");
    const auto &corners = cut->loops.front().vertices;
    check(std::any_of(corners.begin(), corners.end(),
                      [](const auto &v) {
                          return std::abs(v.point.x - 30) < 1.e-9 && std::abs(v.point.y) < 1.e-9;
                      }),
          "The repaired join did not use the shared CAD vertex");
    check(!buildContour(.002, .01) && !buildContour(.000667, .0005),
          "An excessive cut join gap was accepted");
}

void planarCutStock(const std::filesystem::path &folder) {
    const auto blank = BRepPrimAPI_MakeBox(120, 70, 2).Shape();
    const auto hole = [](double depth) {
        return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(25, 20, 3), gp_Dir(0, 0, -1)), 5, depth)
            .Shape();
    };
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
    const gp_Circ bore(gp_Ax2(gp_Pnt(25, 20, -1), gp_Dir(0, 0, 1)), 5);
    BRepBuilderAPI_MakeWire splitBore;
    splitBore.Add(BRepBuilderAPI_MakeEdge(bore, 0, std::numbers::pi / 2).Edge());
    splitBore.Add(BRepBuilderAPI_MakeEdge(bore, std::numbers::pi / 2, 2 * std::numbers::pi).Edge());
    const auto cutter =
        BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(splitBore.Wire()).Face(), gp_Vec(0, 0, 4))
            .Shape();
    const auto plate = BRepAlgoAPI_Cut(blank, cutter).Shape();
    auto document = load(BRepBuilderAPI_Transform(plate, rotation, true).Shape(),
                         folder / "planar-cut-stock.step");
    check(std::count_if(document->faces().begin(), document->faces().end(),
                        [](const auto &face) {
                            return face.surface == application::SurfaceKind::Cylinder;
                        }) == 2,
          "Fixture lost its two cylindrical faces");
    const auto flat = build(document, {});
    const auto &info = flat->info();
    check(info.bends.empty() && std::abs(info.thicknessMm - 2) < 1.e-6 && info.cutProfile &&
              info.cutProfile->loops.size() == 2 &&
              info.cutProfile->loops.front().vertices.size() == 4 &&
              std::abs(info.cutProfile->loops.back().radiusMm - 5) < 1.e-6,
          "Tilted planar stock lost its outline or through hole");
    const auto splitBlank =
        BRepAlgoAPI_Fuse(BRepPrimAPI_MakeBox(40, 70, 2).Shape(),
                         BRepPrimAPI_MakeBox(gp_Pnt(40, 0, 0), 80, 70, 2).Shape())
            .Shape();
    const auto split = load(BRepAlgoAPI_Cut(splitBlank, hole(4)).Shape(),
                            folder / "planar-cut-stock-split-skin.step");
    check(std::none_of(split->faces().begin(), split->faces().end(),
                       [](const auto &face) {
                           return face.surface == application::SurfaceKind::Plane &&
                                  face.areaMm2 > 6000;
                       }),
          "Planar fixture no longer has fragmented skins");
    const auto splitFlat = build(split, {});
    const auto &splitCut = splitFlat->info().cutProfile;
    check(splitCut && splitCut->loops.size() == 2 &&
              std::abs(splitCut->loops.back().radiusMm - 5) < 1.e-6 &&
              std::abs(splitFlat->info().bounds.maximum.x * splitFlat->info().bounds.maximum.y -
                       120 * 70) < 1.e-5,
          "Complete planar section lost some of the stock or its through hole");
    const auto blind = load(BRepAlgoAPI_Cut(blank, hole(2)).Shape(), folder / "planar-pocket.step");
    check(std::holds_alternative<application::FlatPatternError>(
              cad::FlatPatternBuilder{}.build(blind, {{}, {}}, {})),
          "Planar stock with a pocket was accepted as a prismatic cut profile");

    // Curved inner and outer edges of a broad plate can have exactly the
    // same local cylindrical geometry as a short bend.
    const auto rounded = fixtures::bentStrip(16, 30, std::numbers::pi / 2, 3, true, 110);
    const auto boreAt = [](double depth) {
        return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(30, -50, 4), gp_Dir(0, 0, -1)), 5, depth)
            .Shape();
    };
    const auto roundedPlate =
        load(BRepBuilderAPI_Transform(BRepAlgoAPI_Cut(rounded, boreAt(5)).Shape(), rotation, true)
                 .Shape(),
             folder / "planar-rounded-contour.step");
    check(application::isPlanarCutCandidate(roundedPlate->metadata().bendAnalysis),
          "The curved edges of a broad planar stock were reported as a bend");
    const auto roundedFlat = build(roundedPlate, {});
    check(std::abs(roundedFlat->info().thicknessMm - 3) < 1.e-6 && roundedFlat->info().cutProfile &&
              roundedFlat->info().cutProfile->loops.size() == 2,
          "Direct export lost the thickness or through hole of rounded planar stock");
    const auto roundedPocket =
        load(BRepAlgoAPI_Cut(rounded, boreAt(2)).Shape(), folder / "rounded-pocket.step");
    check(!application::isPlanarCutCandidate(roundedPocket->metadata().bendAnalysis),
          "A blind pocket passed the full-body planar proof");
    const auto shortBend = load(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 3),
                                folder / "short-formed-profile.step");
    check(shortBend->metadata().bendAnalysis.bends.size() == 1,
          "A genuine short bend was classified as a cutting contour");
}

void ignoredCutouts(const std::filesystem::path &folder) {
    const auto document = load(fixtures::profiledBend(), folder / "profiled.step");
    const auto source = std::string(document->sourceStep());
    const auto flat = build(document);
    const auto baseline = build(load(fixtures::profiledBend(false), folder / "without-holes.step"));
    check(flat->info().cutoutsIgnored && !baseline->info().cutoutsIgnored &&
              std::abs(flat->info().bounds.maximum.x - baseline->info().bounds.maximum.x) < 1e-5 &&
              std::abs(flat->info().bounds.maximum.y - baseline->info().bounds.maximum.y) < 1e-5 &&
              std::abs(flat->info().bends[0].allowanceMm - baseline->info().bends[0].allowanceMm) <
                  1e-7 &&
              document->sourceStep() == source,
          "Countersink influenced allowance/outer contour or modified the original");
    check(cad::OcctFlatPatternAccess::openingContours(*flat).size() == 3,
          "A hole or one of the two countersink rims was lost/duplicated");
    const auto cuts = flat->info().cutProfile;
    check(cuts && cuts->loops.size() == 3 && cuts->blindOpenings == 0 &&
              cuts->loops.front().vertices.size() >= 5,
          "Laser profile lost a contour or the in-plane chamfer");
    std::vector<double> radii;
    for (const auto &loop : cuts->loops)
        if (loop.radiusMm > 0)
            radii.push_back(loop.radiusMm);
    std::sort(radii.begin(), radii.end());
    check(radii.size() == 2 && std::abs(radii[0] - 1.5) < 1.e-6 && std::abs(radii[1] - 2.5) < 1.e-6,
          "Countersink rim was used as a cut or bore diameter was changed");
    const auto blind = build(load(
        BRepAlgoAPI_Cut(
            fixtures::profiledBend(false),
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(14, -26, 25), gp_Dir(-1, 0, 0)), 1.5, 2).Shape())
            .Shape(),
        folder / "blind-cut.step"));
    check(blind->info().cutProfile && blind->info().cutProfile->blindOpenings == 1 &&
              blind->info().cutProfile->loops.size() == 1,
          "A blind opening was exported as a through cut");
    GProp_GProps props;
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*flat), props);
    const double cutVolume = 55.5 * std::numbers::pi;
    check(std::abs(displayVolume(*flat) - (std::abs(props.Mass()) - cutVolume)) < 1.5,
          "The visible countersink or hole was filled or its wall was distorted");
    std::size_t faces = 0;
    for (TopExp_Explorer e(cad::OcctFlatPatternAccess::shape(*flat), TopAbs_FACE); e.More();
         e.Next())
        ++faces;
    check(faces == document->metadata().faceCount, "The display gained artificial hole caps");
    TopoDS_Shape slot = BRepPrimAPI_MakeBox(gp_Pnt(7, -18, 20), 8, 4, 10).Shape();
    for (double z : {20., 30.})
        slot =
            BRepAlgoAPI_Fuse(
                slot,
                BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(7, -16, z), gp_Dir(1, 0, 0)), 2, 8).Shape())
                .Shape();
    const auto slotted = build(load(BRepAlgoAPI_Cut(fixtures::profiledBend(false), slot).Shape(),
                                    folder / "slot-cut.step"));
    const auto slotCuts = slotted->info().cutProfile;
    check(slotCuts && slotCuts->loops.size() == 2, "A through-slot did not reach the DXF profile");
    const auto &slotVertices = slotCuts->loops.back().vertices;
    double totalSweep = 0;
    for (std::size_t i = 0; i < slotVertices.size(); ++i) {
        const auto &v = slotVertices[i];
        const double sweep = std::abs(4 * std::atan(v.bulge));
        if (sweep == 0)
            continue;
        const auto &next = slotVertices[(i + 1) % slotVertices.size()].point;
        const auto radius =
            std::hypot(v.point.x - next.x, v.point.y - next.y) / (2 * std::sin(sweep / 2));
        check(std::abs(radius - 2) < 1.e-6, "A through-slot arc changed radius");
        totalSweep += sweep;
    }
    check(std::abs(totalSweep - 2 * std::numbers::pi) < 1.e-6,
          "A through-slot lost its exact semicircular ends");
    for (bool through : {true, false}) {
        const auto zone =
            build(load(fixtures::bendWithZoneOpening(through), folder / "zone-opening.step"));
        const auto whole = build(load(fixtures::bentStrip(8, 6), folder / "whole-bend.step"));
        check(zone->info().cutoutsIgnored &&
                  !cad::OcctFlatPatternAccess::openingContours(*zone).empty() &&
                  std::abs(zone->info().bounds.maximum.y - whole->info().bounds.maximum.y) < 1e-5,
              "A bend-zone opening blocked unfolding or changed the blank length");
        check(!zone->info().cutProfile, "An ambiguous bend-zone opening reached the laser file");
    }
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
    const auto rotated =
        build(load(BRepBuilderAPI_Transform(fixtures::profiledBend(), rotation, true).Shape(),
                   folder / "profiled-rotated.step"));
    check(cad::OcctFlatPatternAccess::openingContours(*rotated).size() == 3 &&
              std::abs(displayVolume(*rotated) - displayVolume(*flat)) < .1,
          "Cutout mapping depends on the STEP world orientation");
    check(rotated->info().cutProfile && rotated->info().cutProfile->loops.size() == 3,
          "Laser profile depends on source STEP orientation");
    auto boss = BRepAlgoAPI_Fuse(fixtures::profiledBend(),
                                 BRepPrimAPI_MakeBox(gp_Pnt(12, -20, 30), 5, 5, 5))
                    .Shape();
    const auto rejected =
        cad::FlatPatternBuilder{}.build(load(boss, folder / "hole-and-boss.step"),
                                        {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
    check(std::holds_alternative<application::FlatPatternError>(rejected),
          "Ignoring internal cutouts also discarded unrecognized external material");
}

void trayPatterns(const std::filesystem::path &folder) {
    const auto document = load(fixtures::tray(), folder / "tray.step");
    check(document->metadata().bendAnalysis.bends.size() == 4, "Tray must have four bends");
    const auto bytes = std::string(document->sourceStep());
    const auto flat = build(document);
    const auto &info = flat->info();
    const double ba = std::numbers::pi / 2 * (2 + .38 * 2), extension = 16 + ba;
    const double small = 100 + 2 * extension, large = 200 + 2 * extension;
    const auto dimensionCheck = [&](const auto &p) {
        const auto &b = p->info().bounds.maximum;
        check(std::abs(std::min(b.x, b.y) - small) < 1.e-4 &&
                  std::abs(std::max(b.x, b.y) - large) < 1.e-4,
              "Tray blank dimensions incorrect");
    };
    dimensionCheck(flat);
    const double area = 200 * 100 + 2 * (180 + 80) * extension;
    GProp_GProps props;
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*flat), props);
    check(info.cutoutsIgnored && std::abs(std::abs(props.Mass()) - area * 2) < 1.e-3,
          "Tray area/coverage mismatch");
    check(std::abs(displayVolume(*flat) - (area - 4 * std::numbers::pi * 1.5 * 1.5) * 2) < 2,
          "Tray display filled the holes");
    check(cad::OcctFlatPatternAccess::openingContours(*flat).size() == 4,
          "Tray hole outlines missing");
    std::size_t horizontal = 0, vertical = 0;
    for (const auto &b : info.bends) {
        const auto line = b.centreLine();
        horizontal += std::abs(line[0].y - line[1].y) < 1.e-5;
        vertical += std::abs(line[0].x - line[1].x) < 1.e-5;
        check(std::abs(b.lengthMm() -
                       document->metadata().bendAnalysis.bends[b.bendIndex].lengthMm) < 1.e-5,
              "Bend axis length changed");
        check(std::abs(std::hypot(b.offset.x, b.offset.y) - ba) < 1.e-5,
              "2D allowance vector changed");
    }
    check(horizontal == 2 && vertical == 2, "Tray bend directions collapsed into one axis");
    check(std::all_of(info.bends.begin(), info.bends.end(),
                      [&](const auto &b) { return b.direction == info.bends.front().direction; }),
          "Tray flange directions inconsistent");
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
    dimensionCheck(build(load(BRepBuilderAPI_Transform(fixtures::tray(), rotation, true).Shape(),
                              folder / "tray-rotated.step")));
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 0)));
    dimensionCheck(build(load(BRepBuilderAPI_Transform(fixtures::tray(), mirror, true).Shape(),
                              folder / "tray-mirrored.step")));
    auto angled = build(
        load(fixtures::tray(false, 200, 100, std::numbers::pi / 3), folder / "tray-angled.step"));
    check(angled->info().bends.size() == 4 &&
              std::count_if(angled->info().bends.begin(), angled->info().bends.end(),
                            [](const auto &b) {
                                return std::abs(b.turnRadians - std::numbers::pi / 3) < 1.e-6;
                            }) == 1,
          "Inclined tray wall not unfolded");
    auto largeDoc = load(fixtures::tray(true, 2000, 1000), folder / "tray-large.step");
    check(largeDoc->metadata().closedCutouts.status == application::ClosedCutoutStatus::Filled &&
              std::abs(largeDoc->metadata().closedCutouts.addedVolumeMm3 -
                       4 * std::numbers::pi * 1.5 * 1.5 * 2) < 1.e-3,
          "Small holes on large panel not filled");
    check(build(largeDoc)->info().bends.size() == 4, "Large panel failed");
    const auto branch = build(load(fixtures::branchedTray(), folder / "tray-branch.step"));
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*branch), props);
    check(branch->info().bends.size() == 5 &&
              std::abs(std::abs(props.Mass()) - (area + 12 * extension) * 2) < 1.e-3 &&
              cad::OcctFlatPatternAccess::openingContours(*branch).size() == 4,
          "A bend on a non-root flange lost placement, area or holes");
    const auto oblique = build(load(fixtures::obliqueTray(), folder / "tray-oblique.step"));
    BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::geometry(*oblique), props);
    check(oblique->info().bends.size() == 2 &&
              std::abs(std::abs(props.Mass()) -
                       (35000 + (180 + 100 * std::sqrt(2.) - 20) * extension) * 2) < 1.e-3,
          "Oblique bend axes lost placement or area");
    auto overlap = load(fixtures::concaveTray(), folder / "tray-overlap.step");
    check(overlap->metadata().bendAnalysis.bends.size() == 2,
          "Concave fixture bends not recognized");
    const auto failed =
        cad::FlatPatternBuilder{}.build(overlap, {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
    check(std::holds_alternative<application::FlatPatternError>(failed) &&
              std::get<application::FlatPatternError>(failed).detail == "Flat wall patches overlap",
          "Overlapping non-parallel flanges accepted");
    check(document->sourceStep() == bytes, "Tray source modified");
}

void densePerforation(const std::filesystem::path &folder) {
    constexpr int rows = 11, columns = 12;
    constexpr double t = 2, r = 4, length = 130, flange = 120, hole = 4;
    constexpr double k = .45;
    const auto source = fixtures::bentStrip(r, t, std::numbers::pi / 2, length, true, flange);
    TopTools_ListOfShape arguments, tools;
    arguments.Append(source);
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            tools.Append(
                BRepPrimAPI_MakeBox(gp_Pnt(3, -115 + row * 10, 5 + column * 10), 4, hole, hole)
                    .Shape());
            tools.Append(BRepPrimAPI_MakeCylinder(
                             gp_Ax2(gp_Pnt(-113 + row * 10, 3, 7 + column * 10), gp_Dir(0, 1, 0)),
                             hole / 2, 4)
                             .Shape());
        }
    BRepAlgoAPI_Cut cut;
    cut.SetArguments(arguments);
    cut.SetTools(tools);
    cut.Build();
    check(cut.IsDone() && !cut.HasErrors(), "Dense perforation fixture failed");
    const auto doc = load(cut.Shape(), folder / "dense-perforation.step");
    const auto &metadata = doc->metadata();
    check(metadata.faceCount > 512 &&
              metadata.bendAnalysis.status == application::BendRecognitionStatus::Completed &&
              metadata.bendAnalysis.bends.size() == 1,
          "Dense perforation hid the bend");
    const double removedVolume = rows * columns * hole * hole * (1 + std::numbers::pi / 4) * t;
    check(metadata.thickness.status == application::ThicknessStatus::Unsupported &&
              application::suggestThickness(metadata.thickness) == t &&
              metadata.closedCutouts.status == application::ClosedCutoutStatus::Filled &&
              std::abs(metadata.closedCutouts.addedVolumeMm3 - removedVolume) < 1e-4,
          "Local thickness or verified cutout copy was lost");
    const auto flat = build(doc, {core::SheetMaterial::MildSteel, k, 1});
    const auto &openings = cad::OcctFlatPatternAccess::openingContours(*flat);
    check(flat->info().bends.size() == 1 && flat->info().cutoutsIgnored &&
              openings.size() == 2 * rows * columns,
          "Dense perforation lost its bend or contours on the blank");

    const auto cuts = flat->info().cutProfile;
    check(cuts && cuts->loops.size() == 1 + 2 * rows * columns &&
              std::count_if(cuts->loops.begin(), cuts->loops.end(),
                            [](const auto &loop) { return loop.radiusMm > 0; }) == rows * columns,
          "Perforations are missing from the laser profile or were counted twice");

    application::DrawingRequest request;
    request.unfoldSettings = flat->info().settings;
    request.preparedFlatPattern = flat;
    const auto result = cad::BendingDrawingBuilder{}.build(doc, request, {});
    if (const auto *error = std::get_if<application::DrawingError>(&result))
        throw std::runtime_error(error->detail);
    const auto drawing = std::get<application::BendingDrawingPtr>(result);
    check(drawing->bends.size() == 1 && drawing->classicalViews && drawing->flatPattern,
          "Dense perforation lost its views, bend or flat pattern on the drawing");
    const auto &view = drawing->flatPattern->view;
    const auto matches = [&](bool halfTurn) {
        return std::equal(openings.rbegin(), openings.rend(), view.paths.rbegin(),
                          [&](const auto &contour, const auto &path) {
                              return !path.hidden &&
                                     std::equal(contour.begin(), contour.end(), path.points.begin(),
                                                path.points.end(), [&](auto a, auto b) {
                                                    return halfTurn
                                                               ? b.x == view.maximum.x - a.x &&
                                                                     b.y == view.maximum.y - a.y
                                                               : b.x == a.x && b.y == a.y;
                                                });
                          });
    };
    check(view.paths.size() > openings.size() && (matches(false) || matches(true)),
          "Dense perforation lost closed opening contours on the drawing");
}

int main(int argc, char **argv) {
    try {
        if (argc == 3 && (std::string_view(argv[1]) == "--write-tray" ||
                          std::string_view(argv[1]) == "--write-tray-rotated")) {
            auto shape = fixtures::tray();
            if (std::string_view(argv[1]) == "--write-tray-rotated") {
                gp_Trsf tr;
                tr.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
                shape = BRepBuilderAPI_Transform(shape, tr, true).Shape();
            }
            STEPControl_Writer writer;
            check(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                      writer.Write(argv[2]) == IFSelect_RetDone,
                  "Tray fixture export failed");
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--write-tree") {
            STEPControl_Writer writer;
            check(writer.Transfer(segmentedWall(), STEPControl_AsIs) == IFSelect_RetDone,
                  "Tree transfer failed");
            check(writer.Write(argv[2]) == IFSelect_RetDone, "Tree write failed");
            return 0;
        }
        if (argc > 1) {
            auto d = application::resultDocument(
                cad::StepImporter{}.load(std::filesystem::path(argv[1]), {}, {}));
            auto p = build(d);
            std::cout << "FLAT " << p->info().bounds.maximum.x << " x "
                      << p->info().bounds.maximum.y << " bends " << p->info().bends.size() << '\n';
            return 0;
        }
        const auto folder =
            std::filesystem::temp_directory_path() /
            ("flat-tests-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(folder);
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(p, e);
            }
        } cleanup{folder};
        for (double angle :
             {std::numbers::pi / 4, std::numbers::pi / 2, 3 * std::numbers::pi / 4}) {
            const auto d = load(fixtures::bentStrip(4, 2, angle), folder / "angle.step");
            const auto bytes = std::string(d->sourceStep());
            const auto p = build(d);
            const auto &i = p->info();
            check(std::abs(i.bounds.maximum.x - 80) < 1e-4 &&
                      std::abs(i.bounds.maximum.y - (60 + angle * (4 + .43 * 2))) < 1e-4,
                  "Wrong L blank dimensions");
            check(i.bends.size() == 1 &&
                      std::abs(i.bends[0].allowanceMm - angle * (4 + .43 * 2)) < 1e-5,
                  "Wrong allowance");
            check(d->sourceStep() == bytes, "Original source changed");
            auto changed = build(d, {core::SheetMaterial::Unknown, .5, 1});
            check(std::abs(changed->info().bounds.maximum.y - i.bounds.maximum.y -
                           angle * .07 * 2) < 1e-4,
                  "K did not change the blank");
        }
        trayPatterns(folder);
        treePatterns(folder);
        cyclicReliefPattern(folder);
        manySegmentsOfOneBend(folder);
        coplanarWallPattern(folder);
        splitLinkPattern(folder);
        linearCutJoin();
        planarCutStock(folder);
        ignoredCutouts(folder);
        densePerforation(folder);
        auto channel = load(fixtures::channel(), folder / "channel.step");
        auto p = build(channel);
        check(p->info().bends.size() == 2 &&
                  std::abs(p->info().bounds.maximum.y -
                           (34 + 80 + 34 + std::numbers::pi * (4 + .43 * 2))) < 1e-4,
              "Wrong U blank");
        check(p->info().bends[0].direction == p->info().bends[1].direction,
              "U flange directions differ");
        auto z = build(load(reverseProfile(), folder / "reverse.step"));
        check(z->info().bends.size() == 2 &&
                  z->info().bends[0].direction == -z->info().bends[1].direction,
              "Reverse bend directions lost");
        check(std::abs(z->info().bounds.maximum.y -
                       (30 + 28 + 30 + std::numbers::pi * (4 + .43 * 2))) < 1e-4,
              "Wrong reverse-bend blank length");
        gp_Trsf rotate;
        rotate.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
        auto rotated =
            build(load(BRepBuilderAPI_Transform(fixtures::channel(), rotate, true).Shape(),
                       folder / "rotated.step"));
        check(std::abs(rotated->info().bounds.maximum.y - p->info().bounds.maximum.y) < 1e-4,
              "World rotation changed blank");
        auto hole = BRepAlgoAPI_Cut(fixtures::bentStrip(),
                                    BRepPrimAPI_MakeCylinder(
                                        gp_Ax2(gp_Pnt(0, -15, 40), gp_Dir(1, 0, 0)), 2, 12))
                        .Shape();
        auto holed = build(load(hole, folder / "hole.step"));
        check(std::abs(displayVolume(*holed) -
                       (80 * holed->info().bounds.maximum.y * 2 - std::numbers::pi * 4 * 2)) < .3,
              "Flange hole was filled or distorted");
        auto extra = BRepAlgoAPI_Fuse(fixtures::bentStrip(),
                                      BRepPrimAPI_MakeBox(gp_Pnt(4, -20, 30), 5, 5, 5))
                         .Shape();
        auto r = cad::FlatPatternBuilder{}.build(load(extra, folder / "extra.step"),
                                                 {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
        check(std::holds_alternative<application::FlatPatternError>(r),
              "Unrecognized material silently discarded");
        r = cad::FlatPatternBuilder{}.build(channel, {{core::SheetMaterial::MildSteel, {}, 1}, 3},
                                            {});
        check(std::get<application::FlatPatternError>(r).code ==
                  application::FlatPatternErrorCode::ThicknessConflict,
              "Thickness conflict ignored");
        std::stop_source stop;
        stop.request_stop();
        r = cad::FlatPatternBuilder{}.build(channel, {{core::SheetMaterial::MildSteel, {}, 1}, {}},
                                            stop.get_token());
        check(std::get<application::FlatPatternError>(r).code ==
                  application::FlatPatternErrorCode::Cancelled,
              "Cancellation ignored");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
