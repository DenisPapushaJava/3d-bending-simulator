#include "BendFixtures.h"
#include "TrimmedBend.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <chrono>
#include <iostream>
#include <sstream>
using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}
TopoDS_Shape trim(TopoDS_Shape source, bool compound) {
    const auto plane = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 3), gp_Dir(-.4, .2, 1))).Face();
    source = BRepAlgoAPI_Cut(source, BRepPrimAPI_MakeHalfSpace(plane, gp_Pnt(0, 0, -100)).Solid())
                 .Shape();
    if (compound) {
        const auto flat = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 4), gp_Dir(0, 0, 1))).Face();
        source =
            BRepAlgoAPI_Cut(source, BRepPrimAPI_MakeHalfSpace(flat, gp_Pnt(0, 0, -100)).Solid())
                .Shape();
    }
    require(BRepCheck_Analyzer(source).IsValid(), "Invalid independent oblique fixture");
    return source;
}
gp_Pnt deform(gp_Pnt point, double progress) {
    const double angle = std::atan2(point.Y(), point.X());
    const double radius = std::hypot(point.X(), point.Y());
    if (progress == 0)
        return {5 * angle, point.Z(), radius - 4};
    const double currentRadius = 5 / progress - 1 + radius - 4;
    return {currentRadius * std::cos(angle * progress), currentRadius * std::sin(angle * progress),
            point.Z()};
}
void sectorChecks(bool compound) {
    const auto source = trim(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 80, false), compound);
    std::ostringstream before;
    BRepTools::Write(source, before);
    for (double progress : {1., .5, .1, 0.}) {
        std::cout << "sector compound=" << compound << " progress=" << progress << std::endl;
        const auto mapped = cad::mapTrimmedBend(
            source, [progress](gp_Pnt p) { return deform(p, progress); }, progress, 1e-4);
        require(BRepCheck_Analyzer(mapped).IsValid(), "Invalid mapped oblique sector");
        if (progress == 1) {
            for (bool reverse : {false, true}) {
                BRepAlgoAPI_Cut difference(
                    BRepBuilderAPI_Copy(reverse ? mapped : source, true, false).Shape(),
                    BRepBuilderAPI_Copy(reverse ? source : mapped, true, false).Shape());
                require(difference.IsDone() && !difference.HasErrors() &&
                            volume(difference.Shape()) < 1e-4,
                        "Oblique identity mapping lost or filled material");
            }
        }
        for (double angle : {.1, .5, 1.2})
            for (double radius : {4.1, 4.8, 5.9}) {
                const double x = radius * std::cos(angle), y = radius * std::sin(angle);
                double z = 3 + .4 * x - .2 * y;
                if (compound)
                    z = std::max(4., z);
                const auto expected = deform({x, y, z}, progress);
                BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(expected), mapped);
                require(distance.IsDone() && distance.Value() < 1e-5,
                        "A mapped oblique cap does not follow its actual source surface");
            }
    }
    std::ostringstream after;
    BRepTools::Write(source, after);
    require(before.str() == after.str(), "Mapping mutated source topology or curves");
}
void sheetCheck(const std::filesystem::path &path, bool compound, const gp_Trsf &placement = {}) {
    auto shape = trim(fixtures::bentStrip(), compound);
    shape = BRepBuilderAPI_Transform(shape, placement, true).Shape();
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Oblique fixture export failed");
    const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(doc && doc->metadata().bendAnalysis.bends.size() == 1,
            "Independent oblique bend was not recognized");
    const auto &bend = doc->metadata().bendAnalysis.bends.front();
    require(bend.trimmedEnds && bend.lengthMm >= bend.minimumStraightLengthMm &&
                std::abs(bend.thicknessMm - 2) < 1e-6,
            "Oblique recognition lost its full span or thickness");
    if (!compound) {
        const double sectorArea = 5 * std::numbers::pi;
        const double removed =
            480 + (std::pow(2.2, 3) - std::pow(1.8, 3)) / .48 + 3 * sectorArea + .2 * 152 / 3;
        const double expected = (120 + sectorArea) * 80 - removed;
        require(std::abs(doc->metadata().volumeMm3 - expected) < .001,
                "Oblique fixture disagrees with an independent volume integral");
    }
    const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
    std::ostringstream before;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), before);
    const auto result =
        cad::FlatPatternBuilder{}.build(doc, {{core::SheetMaterial::MildSteel, .5, 1}, {}}, {});
    if (const auto *error = std::get_if<application::FlatPatternError>(&result))
        throw std::runtime_error(error->detail);
    const auto flat = std::get<application::FlatPatternPtr>(result);
    require(flat->info().bends.size() == 1 &&
                BRepCheck_Analyzer(cad::OcctFlatPatternAccess::geometry(*flat)).IsValid(),
            "Oblique sheet lost its flat pattern");
    const auto &cuts = flat->info().cutProfile;
    require(cuts && cuts->midThickness && cuts->loops.size() == 1 &&
                cuts->loops.front().vertices.size() >= 4,
            "The oblique stock did not produce one marked mid-thickness cutting outline");
    const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
    require(layout.walls.size() == 2 && layout.bends.size() == 1, "Oblique layout is incomplete");
    if (!compound) {
        for (double x : {4.1, 5.9}) {
            const gp_Pnt cap = gp_Pnt(x, -10, 5 + .4 * x).Transformed(placement);
            double closest = 1e10;
            for (const auto &wall : layout.walls)
                for (TopExp_Explorer ex(wall.solid, TopAbs_FACE); ex.More(); ex.Next()) {
                    BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(cap),
                                                        ex.Current());
                    require(distance.IsDone(), "Wall boundary distance failed");
                    closest = std::min(closest, distance.Value());
                }
            require(closest < 1e-5, "A beveled wall was replaced by a constant-outline prism");
        }
    }
    if (compound && placement.Form() == gp_Identity) {
        application::RouteDraftRequest request;
        request.operations.push_back(
            {"oblique", 0, {bend.angleRadians, bend.innerRadiusMm}, {}, {}});
        for (std::size_t step : {0, 1}) {
            request.step = step;
            const auto draftResult =
                cad::SingleBendPreviewBuilder{}.buildRouteDraft(doc, request, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&draftResult))
                throw std::runtime_error(error->detail);
            const auto draft = std::get<application::RouteDraftPtr>(draftResult);
            for (std::size_t i = 0; i < draft->visual->frameCount(); ++i)
                require(BRepCheck_Analyzer(cad::OcctPreviewAccess::frame(*draft->visual, i).shape)
                            .IsValid(),
                        "Oblique manual preview published invalid geometry");
        }
    }
    std::ostringstream after;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), after);
    require(before.str() == after.str(), "Unfolding mutated its imported source");
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("oblique-bend-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    int status = 0;
    try {
        sectorChecks(false);
        sectorChecks(true);
        sheetCheck(folder / "single.step", false);
        sheetCheck(folder / "compound.step", true);
        gp_Trsf rotate;
        rotate.SetRotation(gp_Ax1(gp_Pnt(20, -30, 40), gp_Dir(1, 2, 3)), .71);
        sheetCheck(folder / "rotated.step", true, rotate);
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(gp_Pnt(30, 20, -10), gp_Dir(1, 2, 3)));
        sheetCheck(folder / "mirrored.step", true, mirror);
        for (bool boss : {false, true}) {
            auto shape = trim(fixtures::bentStrip(), true);
            shape = boss ? BRepAlgoAPI_Fuse(
                               shape, BRepPrimAPI_MakeBox(gp_Pnt(5.5, -20, 30), 2, 5, 5).Shape())
                               .Shape()
                         : BRepAlgoAPI_Cut(
                               shape, BRepPrimAPI_MakeBox(gp_Pnt(5.5, -35, 30), 2, 15, 10).Shape())
                               .Shape();
            const auto path = folder / (boss ? "boss.step" : "open-pocket.step");
            STEPControl_Writer writer;
            require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(path.string().c_str()) == IFSelect_RetDone,
                    "Negative fixture export failed");
            const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
            require(doc && doc->metadata().bendAnalysis.bends.size() == 1 &&
                        doc->metadata().closedCutouts.status !=
                            application::ClosedCutoutStatus::Filled,
                    "Negative oblique fixture did not retain its actual extra material/pocket");
            const auto result = cad::FlatPatternBuilder{}.build(
                doc, {{core::SheetMaterial::MildSteel, .5, 1}, {}}, {});
            const auto *error = std::get_if<application::FlatPatternError>(&result);
            require(
                error && error->code == application::FlatPatternErrorCode::UnsupportedGeometry,
                "Actual-wall fallback accepted a boss or variable-thickness pocket as sheet stock");
        }
        std::cout << "Oblique caps, full reconstruction and flat patterns passed\n";
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
