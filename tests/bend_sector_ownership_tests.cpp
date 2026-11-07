#include "BendOperation.h"
#include "BendSectorOwnership.h"
#include "SheetGeometry.h"
#include "TrayFixtures.h"
#include "TrimmedBend.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <Geom2d_Curve.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <chrono>
#include <gp_Vec2d.hxx>
#include <iostream>
#include <sstream>

using namespace bending;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}
TopoDS_Shape compound(const TopoDS_Shape &first, const TopoDS_Shape &second) {
    TopoDS_Compound result;
    BRep_Builder builder;
    builder.MakeCompound(result);
    builder.Add(result, first);
    builder.Add(result, second);
    return result;
}
std::array<TopoDS_Face, 2> skins(const TopoDS_Shape &sector) {
    std::array<TopoDS_Face, 2> result;
    std::size_t index = 0;
    for (TopExp_Explorer ex(sector, TopAbs_FACE); ex.More(); ex.Next())
        if (BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType() == GeomAbs_Cylinder) {
            require(index < result.size(), "Unexpected fixture cylinder");
            result[index++] = TopoDS::Face(ex.Current());
        }
    require(index == result.size(), "Missing fixture skins");
    return result;
}
void ownershipChecks() {
    const auto first = fixtures::bentStrip(2, 2, std::numbers::pi / 2, 40, false);
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(10, 0, 0));
    const auto second = BRepBuilderAPI_Transform(first, shift, true).Shape();
    const auto source = compound(first, second);
    const auto envelope = BRepPrimAPI_MakeBox(gp_Pnt(-1, -1, -1), 16, 6, 42).Shape();
    const auto anchors = skins(first);
    const auto selected = cad::ownedBendSector(source, envelope, anchors, 1e-4, {});
    require(selected && std::abs(volume(*selected) - volume(first)) < 1e-6,
            "Complete real skins did not select their unique component");
    const auto foreign = skins(second);
    require(!cad::ownedBendSector(source, envelope, {anchors[0], foreign[1]}, 1e-4, {}),
            "Skins in different components were assigned to one bend");
    const auto partial = BRepPrimAPI_MakeBox(gp_Pnt(-1, -1, 1), 16, 6, 38).Shape();
    require(!cad::ownedBendSector(source, partial, anchors, 1e-4, {}),
            "Partial source-skin coverage was accepted");
    const auto split =
        BRepAlgoAPI_Cut(envelope, BRepPrimAPI_MakeBox(gp_Pnt(-2, -2, 19), 20, 10, 2).Shape())
            .Shape();
    require(!cad::ownedBendSector(source, split, anchors, 1e-4, {}),
            "A split real skin was silently assigned to just one component");
    const auto unrelated = BRepBuilderAPI_Copy(first, true, false).Shape();
    require(!cad::ownedBendSector(source, envelope, skins(unrelated), 1e-4, {}),
            "Unrelated but coincident source faces were guessed by geometry");
    std::stop_source cancelled;
    cancelled.request_stop();
    require(!cad::ownedBendSector(source, envelope, anchors, 1e-4, cancelled.get_token()),
            "Cancelled ownership selection published a sector");
}
void vertexUncertaintyCheck() {
    const auto source = fixtures::bentStrip(2, 2, std::numbers::pi / 2, 40, false);
    const auto anchor = skins(source)[0];
    bool changed = false;
    BRep_Builder builder;
    for (BRepTools_WireExplorer ex(BRepTools::OuterWire(anchor), anchor); ex.More(); ex.Next()) {
        double first, last;
        const auto curve = BRep_Tool::CurveOnSurface(ex.Current(), anchor, first, last);
        if (std::abs(curve->Value(last).X() - curve->Value(first).X()) > 1e-8)
            continue;
        const auto shifted = Handle(Geom2d_Curve)::DownCast(curve->Copy());
        shifted->Translate(gp_Vec2d(0, 4e-6));
        builder.UpdateEdge(ex.Current(), shifted, anchor, 1e-5);
        builder.UpdateVertex(TopExp::FirstVertex(ex.Current()), 1e-5);
        builder.UpdateVertex(TopExp::LastVertex(ex.Current()), 1e-5);
        changed = true;
        break;
    }
    require(changed && BRepCheck_Analyzer(source).IsValid(), "Invalid tolerance-bearing fixture");
    std::ostringstream before;
    BRepTools::Write(source, before);
    for (double progress : {1., .5, 0.}) {
        const auto mapped = cad::mapTrimmedBend(
            source,
            [progress](gp_Pnt p) {
                const double angle = std::atan2(p.Y(), p.X());
                const double radius = std::hypot(p.X(), p.Y());
                if (progress == 0)
                    return gp_Pnt(3 * angle, p.Z(), radius - 2);
                const double current = 3 / progress + radius - 3;
                return gp_Pnt(current * std::cos(progress * angle),
                              current * std::sin(progress * angle), p.Z());
            },
            progress, 1e-4);
        require(BRepCheck_Analyzer(mapped).IsValid() &&
                    std::abs(volume(mapped) - volume(source)) < 1e-3,
                "Lost source endpoint uncertainty in identity/motion/flat mapping");
    }
    bool rejected = false;
    try {
        (void)cad::mapTrimmedBend(source, [](gp_Pnt p) { return p; }, 1, 1e-6);
    } catch (const cad::TrimmedBendError &) {
        rejected = true;
    }
    require(rejected, "Derived vertex tolerance exceeded comparison limit");
    std::ostringstream after;
    BRepTools::Write(source, after);
    require(before.str() == after.str(), "Mapping changed source uncertainty or pcurves");
}
TopoDS_Shape miteredSide(gp_Pnt origin, gp_Vec axis, gp_Vec outward, double length) {
    constexpr double gap = .2;
    auto side = fixtures::traySide(origin.Translated(axis * -4), axis, outward, length + 8);
    const auto clip = [&](gp_Pnt point, gp_Dir normal, gp_Pnt outside) {
        const auto plane = BRepBuilderAPI_MakeFace(gp_Pln(point, normal)).Face();
        side = BRepAlgoAPI_Cut(side, BRepPrimAPI_MakeHalfSpace(plane, outside).Solid()).Shape();
    };
    clip(origin.Translated(axis * gap), gp_Dir(axis + outward), origin.Translated(axis * -100));
    clip(origin.Translated(axis * (length - gap)), gp_Dir(axis - outward),
         origin.Translated(axis * (length + 100)));
    return side;
}
TopoDS_Shape miteredTray() {
    auto source = BRepPrimAPI_MakeBox(120, 80, 2).Shape();
    for (const auto &side : {miteredSide({0, 0, 0}, {1, 0, 0}, {0, -1, 0}, 120),
                             miteredSide({0, 80, 0}, {1, 0, 0}, {0, 1, 0}, 120),
                             miteredSide({0, 0, 0}, {0, 1, 0}, {-1, 0, 0}, 80),
                             miteredSide({120, 0, 0}, {0, 1, 0}, {1, 0, 0}, 80)})
        source = BRepAlgoAPI_Fuse(source, side).Shape();
    ShapeUpgrade_UnifySameDomain unify(source, true, true, true);
    unify.Build();
    require(BRepCheck_Analyzer(unify.Shape()).IsValid(), "Invalid independent mitered tray");
    return unify.Shape();
}
void cornerCaptureCheck() {
    const auto source = miteredTray();
    gp_Trsf placement;
    placement.SetValues(0, 0, 1, -3.8, -1, 0, 0, 0, 0, -1, 0, 4);
    const auto envelope =
        BRepBuilderAPI_Transform(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 127.6, false),
                                 placement, true)
            .Shape();
    BRepAlgoAPI_Common common(BRepBuilderAPI_Copy(source, true, false).Shape(), envelope);
    TopTools_IndexedMapOfShape parts;
    TopExp::MapShapes(common.Shape(), TopAbs_SOLID, parts);
    require(parts.Extent() > 1, "Corner fixture no longer exercises neighbour capture");
    std::vector<TopoDS_Face> anchors;
    for (TopExp_Explorer ex(source, TopAbs_FACE); ex.More(); ex.Next()) {
        const auto face = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Cylinder)
            continue;
        const auto cylinder = surface.Cylinder();
        if (cylinder.Axis().Direction().IsParallel(gp_Dir(1, 0, 0), 1e-7) &&
            std::abs(cylinder.Location().Y()) < 1e-6 &&
            std::abs(cylinder.Location().Z() - 4) < 1e-6)
            anchors.push_back(face);
    }
    require(anchors.size() == 2 &&
                cad::ownedBendSector(source, envelope, {anchors[0], anchors[1]}, 1e-4, {}),
            "Actual mitered corner has no unique full-skin owner");
}
application::DocumentPtr load(const std::filesystem::path &path, const TopoDS_Shape &shape) {
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Cannot export owned corner fixture");
    const auto result = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(bool(result), "Cannot import owned corner fixture");
    return result;
}
void trayChecks(const std::filesystem::path &path, gp_Trsf placement = {}) {
    const auto shape = BRepBuilderAPI_Transform(miteredTray(), placement, true).Shape();
    const auto doc = load(path, shape);
    require(doc->metadata().bendAnalysis.bends.size() == 4, "Lost mitered tray bends");
    const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
    std::ostringstream before;
    BRepTools::Write(cad::OcctDocumentAccess::simulationShape(*native), before);
    const auto result =
        cad::FlatPatternBuilder{}.build(doc, {{core::SheetMaterial::MildSteel, .45, 1}, {}}, {});
    if (const auto *error = std::get_if<application::FlatPatternError>(&result))
        throw std::runtime_error(error->detail);
    const auto flat = std::get<application::FlatPatternPtr>(result);
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    require(layout.walls.size() == 5 && layout.bends.size() == 4 &&
                BRepCheck_Analyzer(cad::OcctFlatPatternAccess::geometry(*flat)).IsValid(),
            "Incomplete mitered tray decomposition");
    // The flat web plus four 16-mm straight walls and analytical BA.
    const double extension = 2 * (16 + (2 + .45 * 2) * std::numbers::pi / 2);
    const auto &bounds = flat->info().bounds;
    const double x = bounds.maximum.x - bounds.minimum.x;
    const double y = bounds.maximum.y - bounds.minimum.y;
    require(std::abs(std::max(x, y) - (120 + extension)) < 1e-4 &&
                std::abs(std::min(x, y) - (80 + extension)) < 1e-4,
            "Mitered tray flat dimensions differ from independent allowance");
    TopoDS_Shape reconstruction;
    for (const auto &wall : layout.walls)
        reconstruction = reconstruction.IsNull()
                             ? wall.solid
                             : BRepAlgoAPI_Fuse(reconstruction, wall.solid).Shape();
    for (const auto &bend : layout.bends)
        reconstruction = BRepAlgoAPI_Fuse(reconstruction, bend.solid).Shape();
    for (bool reverse : {false, true}) {
        const auto &source = cad::OcctDocumentAccess::simulationShape(*native);
        BRepAlgoAPI_Cut difference(
            BRepBuilderAPI_Copy(reverse ? reconstruction : source, true, false).Shape(),
            BRepBuilderAPI_Copy(reverse ? source : reconstruction, true, false).Shape());
        require(difference.IsDone() && !difference.HasErrors() && volume(difference.Shape()) < 1e-4,
                "Actual corner material was dropped or duplicated");
    }
    const auto geometry = cad::prepareSheetGeometry(doc, {});
    const auto *verified = std::get_if<cad::VerifiedSheetGeometry>(&geometry);
    require(verified && verified->source == doc && verified->layout->walls.size() == 5 &&
                verified->layout->bends.size() == 4,
            "Simulation did not receive the complete unfolding partition");
    const auto &bend = doc->metadata().bendAnalysis.bends.front();
    for (bool reversed : {false, true}) {
        const auto operation = cad::prepareBendOperation(
            doc, 0, {bend.angleRadians, bend.innerRadiusMm}, reversed, {}, {});
        const auto *prepared = std::get_if<cad::PreparedBendOperationPtr>(&operation);
        require(prepared, "Simulation cannot prepare the fully owned corner partition");
        const auto cycle = (*prepared)->at(0, {});
        if (const auto *error = std::get_if<application::BendPreviewError>(&cycle))
            require(error->code == application::BendPreviewErrorCode::UnsupportedGeometry &&
                        error->detail.find("preserve the volume") != std::string::npos,
                    "Owned corner failed before its independent kinematic volume check");
        else
            require(std::get<application::BendPreviewPtr>(cycle)->frameCount() == 21,
                    "Incomplete intrinsic corner cycle");
    }
    std::ostringstream after;
    BRepTools::Write(cad::OcctDocumentAccess::simulationShape(*native), after);
    require(before.str() == after.str(), "Ownership/unfolding mutated source BRep");
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bend-sector-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    int status = 0;
    try {
        ownershipChecks();
        vertexUncertaintyCheck();
        cornerCaptureCheck();
        trayChecks(folder / "mitered.step");
        gp_Trsf rotate;
        rotate.SetRotation(gp_Ax1(gp_Pnt(20, -30, 40), gp_Dir(1, 2, 3)), .71);
        trayChecks(folder / "rotated.step", rotate);
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(gp_Pnt(30, 20, -10), gp_Dir(1, 2, 3)));
        trayChecks(folder / "mirrored.step", mirror);
        // Closed pockets retain the explicitly authorised D029 approximation;
        // the open-pocket case below must not silently enter that path.
        const auto closedPocket =
            BRepAlgoAPI_Cut(miteredTray(), BRepPrimAPI_MakeBox(gp_Pnt(40, 30, 1), 5, 5, 3).Shape())
                .Shape();
        const auto filledDoc = load(folder / "closed-pocket.step", closedPocket);
        require(filledDoc->metadata().closedCutouts.status ==
                    application::ClosedCutoutStatus::Filled,
                "Closed cutout approximation changed");
        const auto filledFlat = cad::FlatPatternBuilder{}.build(
            filledDoc, {{core::SheetMaterial::MildSteel, .45, 1}, {}}, {});
        const auto *filled = std::get_if<application::FlatPatternPtr>(&filledFlat);
        require(filled && (*filled)->info().cutoutsIgnored &&
                    !cad::OcctFlatPatternAccess::openingContours(**filled).empty(),
                "Closed cutouts lost their approximation status or visible contour");
        for (bool boss : {false, true}) {
            const auto cutter = boss ? BRepPrimAPI_MakeBox(gp_Pnt(40, 30, 1), 5, 5, 3).Shape()
                                     : BRepPrimAPI_MakeBox(gp_Pnt(40, -3, 15), 5, 3, 10).Shape();
            const auto source = boss ? BRepAlgoAPI_Fuse(miteredTray(), cutter).Shape()
                                     : BRepAlgoAPI_Cut(miteredTray(), cutter).Shape();
            const auto doc = load(folder / (boss ? "boss.step" : "pocket.step"), source);
            require(doc->metadata().bendAnalysis.bends.size() == 4 &&
                        doc->metadata().closedCutouts.status !=
                            application::ClosedCutoutStatus::Filled,
                    "Negative fixture lost its bends or actual extra surface");
            const auto result = cad::FlatPatternBuilder{}.build(
                doc, {{core::SheetMaterial::MildSteel, .45, 1}, {}}, {});
            const auto *error = std::get_if<application::FlatPatternError>(&result);
            require(error && error->code == application::FlatPatternErrorCode::UnsupportedGeometry,
                    "An extra volume or variable thickness pocket was silently accepted");
            require(std::holds_alternative<application::FlatPatternError>(
                        cad::prepareSheetGeometry(doc, {})),
                    "Geometry-only partition dropped an unknown wall or extra volume");
        }
        std::cout << "Owned corner sectors and full source reconstruction passed\n";
    } catch (const Standard_Failure &error) {
        std::cerr << error.GetMessageString() << '\n';
        status = 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::filesystem::remove_all(folder);
    return status;
}
