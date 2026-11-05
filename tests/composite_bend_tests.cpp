#include "BendFixtures.h"
#include "CompositeBendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Line.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Solid.hxx>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
application::DocumentPtr load(const TopoDS_Shape &shape, const std::filesystem::path &path) {
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Fixture export failed");
    const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(bool(doc), "Fixture import failed");
    return doc;
}
TopoDS_Shape approximateEnd() {
    auto shape = composite_fixture::part();
    bool changed = false;
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
        const auto edge = TopoDS::Edge(ex.Current());
        if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_BSplineCurve)
            continue;
        double first, last;
        const auto curve = BRep_Tool::Curve(edge, first, last);
        if (curve.IsNull())
            continue;
        // Keep the exact cylindrical surface and UV end, with an imperfect 3D
        // end curve as seen in neutral CAD exports. Never reduce its tolerance.
        auto shifted = Handle(Geom_BSplineCurve)::DownCast(curve->Copy());
        if (shifted.IsNull() || shifted->NbPoles() < 3)
            continue;
        for (int at = 2; at < shifted->NbPoles(); ++at) {
            auto pole = shifted->Pole(at);
            const double radius = std::hypot(pole.X(), pole.Y());
            require(radius > 0, "Degenerate end-curve pole");
            pole.SetX(pole.X() * (1 + .004 / radius));
            pole.SetY(pole.Y() * (1 + .004 / radius));
            shifted->SetPole(at, pole);
        }
        BRep_Builder builder;
        builder.UpdateEdge(edge, shifted, .006);
        for (TopExp_Explorer v(edge, TopAbs_VERTEX); v.More(); v.Next())
            builder.UpdateVertex(TopoDS::Vertex(v.Current()), .006);
        changed = true;
        break;
    }
    require(changed && BRepCheck_Analyzer(shape).IsValid(),
            "Invalid approximate end-curve fixture");
    return shape;
}
void check(const application::DocumentPtr &doc) {
    const auto &bends = doc->metadata().bendAnalysis.bends;
    std::cout << "recognized=" << bends.size() << " faces=" << doc->metadata().faceCount;
    for (const auto &b : bends)
        std::cout << " length=" << b.lengthMm << " core=" << b.minimumStraightLengthMm
                  << " trimmed=" << b.trimmedEnds;
    std::cout << std::endl;
    require(bends.size() == 1 && bends[0].trimmedEnds &&
                std::abs(bends[0].minimumStraightLengthMm - 76) < 1e-4,
            "Composite cap or its common span was not recognized");
    const auto bytes = std::string(doc->sourceStep());
    const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
    std::ostringstream sourceGeometry;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), sourceGeometry);
    const double allowance = 5 * std::numbers::pi / 2;
    const double expected = 2 * (80 * (60 + allowance) - 120 - allowance);
    require(std::abs(doc->metadata().volumeMm3 - expected) < .002,
            "Fixture disagrees with independent developed area");
    const auto flatResult =
        cad::FlatPatternBuilder{}.build(doc, {{core::SheetMaterial::MildSteel, .5, 1}, {}}, {});
    if (const auto *e = std::get_if<application::FlatPatternError>(&flatResult))
        throw std::runtime_error(e->detail);
    const auto flat = std::get<application::FlatPatternPtr>(flatResult);
    require(std::abs(volume(cad::OcctFlatPatternAccess::geometry(*flat)) - expected) < .002,
            "Flattening filled or lost a cut end");
    const auto &bend = bends[0];
    cad::SingleBendPreviewBuilder builder;
    for (bool side : {false, true}) {
        const auto result =
            builder.build(doc, 0, {bend.angleRadians, bend.innerRadiusMm}, side, {});
        if (const auto *e = std::get_if<application::BendPreviewError>(&result))
            throw std::runtime_error(e->detail);
        const auto preview = std::get<application::BendPreviewPtr>(result);
        for (std::size_t i = 0; i < preview->frameCount(); ++i) {
            const auto &frame = cad::OcctPreviewAccess::frame(*preview, i);
            require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                        std::abs(volume(frame.shape) - expected) < .002 &&
                        !frame.displayShape.IsNull(),
                    "A deformed end lost material or its skin");
        }
    }
    auto assignment = tool_fixture::pair(16, .8);
    application::RouteDraftRequest request;
    request.withTools = true;
    request.operations.push_back({"bend",
                                  0,
                                  {bend.angleRadians, bend.innerRadiusMm},
                                  core::toolParameters(assignment),
                                  assignment});
    for (std::size_t step : {0, 1}) {
        request.step = step;
        const auto result = builder.buildRouteDraft(doc, request, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error(e->detail);
        const auto draft = std::get<application::RouteDraftPtr>(result);
        require(draft->checkedToolPositions == draft->visual->frameCount(),
                "A trimmed position bypassed tool checking");
        for (std::size_t i = 0; i < draft->visual->frameCount(); ++i) {
            const auto &f = cad::OcctPreviewAccess::frame(*draft->visual, i);
            bool collision = false;
            for (const auto &tool : f.tools) {
                BRepAlgoAPI_Common common(f.shape, tool);
                require(common.IsDone() && !common.HasErrors(), "Independent collision failed");
                collision |= volume(common.Shape()) >
                             80 * 2 * doc->metadata().bendAnalysis.lengthToleranceMm * 2;
            }
            require(collision == draft->visual->collisionAtFrame(i).has_value(),
                    "Trimmed-end collision differs from full-body Boolean");
        }
    }
    application::BendRouteRequest route;
    route.operations = request.operations;
    route.withTools = true;
    route.allowPartialEndSupport = true;
    route.maximumEndSupportLossMm = 3;
    const auto resultRoute = builder.buildRoute(doc, route, {});
    if (const auto *e = std::get_if<application::BendRouteError>(&resultRoute))
        throw std::runtime_error(e->detail);
    require(std::get<application::BendRoutePtr>(resultRoute)->steps.size() == 1,
            "Full route lost a trimmed operation");
    assignment = tool_fixture::pair(16, .8, 80);
    request.operations[0].assignment = assignment;
    request.operations[0].tools = core::toolParameters(assignment);
    request.step = 1;
    const auto conflict = builder.buildRouteDraft(doc, request, {});
    require(std::holds_alternative<application::RouteDraftPtr>(conflict),
            "The conflicting configuration could not be inspected");
    const auto visual = std::get<application::RouteDraftPtr>(conflict)->visual;
    bool found = false;
    for (std::size_t i = 0; i < visual->frameCount(); ++i)
        if (visual->collisionAtFrame(i) == application::ToolCollisionPair::SheetPunch) {
            found = true;
            require(!cad::OcctPreviewAccess::frame(*visual, i).collisionHighlights.empty(),
                    "A detected collision lost its contact locations");
        }
    require(found, "A broad punch passed through the trimmed part without a conflict");
    require(doc->sourceStep() == bytes, "Source STEP changed");
    std::ostringstream afterGeometry;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), afterGeometry);
    require(afterGeometry.str() == sourceGeometry.str(), "Source topology or curves changed");
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("composite-bend-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    int status = 0;
    try {
        const auto shape = composite_fixture::part();
        check(load(shape, folder / "source.step"));
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(gp_Pnt(10, 20, 30), gp_Dir(1, 2, 3)));
        check(load(BRepBuilderAPI_Transform(shape, mirror, true), folder / "mirrored.step"));
        const auto tolerant = load(approximateEnd(), folder / "approximate-end.step");
        std::cout << "approximate BRep tolerance=" << tolerant->metadata().maximumToleranceMm
                  << std::endl;
        require(tolerant->metadata().maximumToleranceMm > .001 &&
                    tolerant->metadata().maximumToleranceMm <= .01,
                "STEP lost the approximate end-curve tolerance");
        require(tolerant->metadata().bendAnalysis.lengthToleranceMm <= .01,
                "Approximate end widened the downstream collision tolerance");
        require(tolerant->metadata().bendAnalysis.bends.size() == 1,
                "Bounded BRep tolerance prevented analytic recognition");
        // This deliberately inconsistent end must still pass the complete
        // reconstruction before any flat/draft can be published. Recognizing
        // its analytic cylinders alone must not bypass that proof.
        const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(tolerant);
        std::ostringstream before;
        BRepTools::Write(cad::OcctDocumentAccess::shape(*native), before);
        const auto bytes = std::string(tolerant->sourceStep());
        const auto flat = cad::FlatPatternBuilder{}.build(
            tolerant, {{core::SheetMaterial::MildSteel, .5, 1}, {}}, {});
        const auto *flatError = std::get_if<application::FlatPatternError>(&flat);
        require(flatError &&
                    flatError->code == application::FlatPatternErrorCode::UnsupportedGeometry,
                "Recognition bypassed the full original-solid reconstruction");
        application::RouteDraftRequest request;
        const auto &bend = tolerant->metadata().bendAnalysis.bends.front();
        request.operations.push_back({"bend", 0, {bend.angleRadians, bend.innerRadiusMm}, {}, {}});
        request.step = 1;
        const auto draft = cad::SingleBendPreviewBuilder{}.buildRouteDraft(tolerant, request, {});
        const auto *draftError = std::get_if<application::BendRouteError>(&draft);
        require(draftError && draftError->code == application::BendRouteErrorCode::Unsupported,
                "A failed full reconstruction published a manual draft");
        std::ostringstream after;
        BRepTools::Write(cad::OcctDocumentAccess::shape(*native), after);
        require(before.str() == after.str() && bytes == tolerant->sourceStep(),
                "Failed tolerant reconstruction altered its source");
    } catch (const Standard_Failure &e) {
        std::cerr << e.GetMessageString() << '\n';
        status = 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    std::filesystem::remove_all(folder);
    return status;
}
