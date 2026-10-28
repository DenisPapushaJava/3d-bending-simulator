#include "BendFixtures.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
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
        check(std::abs(b.maximumX - b.minimumX -
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
                  "Closed or disconnected sheet graph",
          "Closed sheet was split without a seam");
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
    for (bool through : {true, false}) {
        const auto zone =
            build(load(fixtures::bendWithZoneOpening(through), folder / "zone-opening.step"));
        const auto whole = build(load(fixtures::bentStrip(8, 6), folder / "whole-bend.step"));
        check(zone->info().cutoutsIgnored &&
                  !cad::OcctFlatPatternAccess::openingContours(*zone).empty() &&
                  std::abs(zone->info().bounds.maximum.y - whole->info().bounds.maximum.y) < 1e-5,
              "A bend-zone opening blocked unfolding or changed the blank length");
    }
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(2, 3, 4), gp_Dir(1, 2, 3)), .71);
    const auto rotated =
        build(load(BRepBuilderAPI_Transform(fixtures::profiledBend(), rotation, true).Shape(),
                   folder / "profiled-rotated.step"));
    check(cad::OcctFlatPatternAccess::openingContours(*rotated).size() == 3 &&
              std::abs(displayVolume(*rotated) - displayVolume(*flat)) < .1,
          "Cutout mapping depends on the STEP world orientation");
    auto boss = BRepAlgoAPI_Fuse(fixtures::profiledBend(),
                                 BRepPrimAPI_MakeBox(gp_Pnt(12, -20, 30), 5, 5, 5))
                    .Shape();
    const auto rejected =
        cad::FlatPatternBuilder{}.build(load(boss, folder / "hole-and-boss.step"),
                                        {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
    check(std::holds_alternative<application::FlatPatternError>(rejected),
          "Ignoring internal cutouts also discarded unrecognized external material");
}

int main(int argc, char **argv) {
    try {
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
        treePatterns(folder);
        ignoredCutouts(folder);
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
