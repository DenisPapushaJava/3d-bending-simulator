#include "BendFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace bending;
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}
struct Temporary {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("bend-preview-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() {
        std::filesystem::create_directory(path);
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
application::DocumentPtr load(const TopoDS_Shape &shape, const std::filesystem::path &path,
                              const char *unit = "MM") {
    Interface_Static::SetCVal("write.step.unit", unit);
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone,
            "Cannot transfer fixture");
    require(writer.Write(path.string().c_str()) == IFSelect_RetDone, "Cannot write fixture");
    const auto loaded = cad::StepImporter{}.load(path, {}, {});
    const auto document = application::resultDocument(loaded);
    require(document != nullptr, "Cannot import preview fixture");
    return document;
}
application::BendPreviewPtr preview(application::DocumentPtr document, bool fixedSecond) {
    const auto &bend = document->metadata().bendAnalysis.bends.at(0);
    const auto result = cad::SingleBendPreviewBuilder{}.build(
        document, 0, {bend.angleRadians, bend.innerRadiusMm}, fixedSecond, {});
    if (const auto *error = std::get_if<application::BendPreviewError>(&result)) {
        throw std::runtime_error("Preview failed: " + error->detail);
    }
    return std::get<application::BendPreviewPtr>(result);
}
void check(const application::BendPreviewPtr &result, double sourceVolume) {
    require(result && result->frameCount() == 21, "Missing preview states");
    double fixedVolume = 0;
    for (std::size_t i = 0; i < result->frameCount(); ++i) {
        const auto &frame = cad::OcctPreviewAccess::frame(*result, i);
        require(std::abs(volume(frame.shape) - sourceVolume) < 1.0e-4,
                "Intermediate volume differs from CAD");
        if (i == 0) {
            fixedVolume = volume(frame.regions[0]);
        }
        require(std::abs(volume(frame.regions[0]) - fixedVolume) < 1.0e-5,
                "Fixed flange volume changed");
        for (const auto &region : frame.regions) {
            require(BRepCheck_Analyzer(region).IsValid(), "Invalid region");
        }
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(frame.shape, TopAbs_FACE, faces);
        for (int j = 1; j <= faces.Extent(); ++j) {
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(faces(j)), location);
            require(!mesh.IsNull() && mesh->NbTriangles() > 0, "Unmeshed preview face");
        }
    }
}
application::BendPreviewPtr checkMotion(application::DocumentPtr document, core::PressTools tools) {
    const auto &bend = document->metadata().bendAnalysis.bends.at(0);
    const auto result = cad::SingleBendPreviewBuilder{}.buildToolMotion(
        document, 0, {bend.angleRadians, bend.innerRadiusMm}, tools, {});
    if (const auto *failure = std::get_if<application::BendPreviewError>(&result)) {
        throw std::runtime_error("Motion failed at frame " +
                                 std::to_string(failure->frameIndex.value_or(0)) + ": " +
                                 failure->detail);
    }
    const auto motion = std::get<application::BendPreviewPtr>(result);
    require(motion->frameCount() == 61 && motion->pressMotion() && motion->toolSetup(),
            "Missing press motion frames");
    double firstFixedY = 0, lastFixedY = 0;
    double previousTip = 1e10;
    for (std::size_t i = 0; i < motion->frameCount(); ++i) {
        const auto &frame = cad::OcctPreviewAccess::frame(*motion, i);
        const auto *state = motion->pressState(i);
        require(state && BRepCheck_Analyzer(frame.shape).IsValid(), "Invalid press frame");
        double total = 0;
        for (const auto &region : frame.regions) {
            total += volume(region);
        }
        require(std::abs(total - document->metadata().volumeMm3) < 1e-4,
                "Contact motion changed material volume");
        BRepExtrema_DistShapeShape left(frame.regions[0], frame.tools[0]),
            right(frame.regions[2], frame.tools[0]);
        BRepExtrema_DistShapeShape punch(frame.regions[1], frame.tools[1]);
        require(left.IsDone() && right.IsDone() && punch.IsDone() && left.Value() < 1e-5 &&
                    right.Value() < 1e-5,
                "Geometric shoulder contact missing");
        const double gap = state->phase == simulation::PressPhase::Approach
                               ? state->punchTipYmm - motion->model().thicknessMm
                               : 0;
        require(state->phase == simulation::PressPhase::Retracting ||
                    std::abs(punch.Value() - gap) < 1e-5,
                "Punch missed expected contact/clearance");
        require(state->phase == simulation::PressPhase::Retracting
                    ? state->punchTipYmm >= previousTip - 1e-7
                    : state->punchTipYmm <= previousTip + 1e-7,
                "Punch reversed inside a stroke phase");
        if (i > 40) {
            const auto &formed = cad::OcctPreviewAccess::frame(*motion, 40);
            for (std::size_t region = 0; region < 3; ++region) {
                GProp_GProps a, b;
                BRepGProp::VolumeProperties(formed.regions[region], a);
                BRepGProp::VolumeProperties(frame.regions[region], b);
                require(a.CentreOfMass().Distance(b.CentreOfMass()) < 1e-7,
                        "Punch return moved or unfolded the sheet");
            }
        }
        previousTip = state->punchTipYmm;
        GProp_GProps properties;
        BRepGProp::VolumeProperties(frame.regions[0], properties);
        if (i == 0) {
            firstFixedY = properties.CentreOfMass().Y();
        }
        if (i == 40) {
            lastFixedY = properties.CentreOfMass().Y();
        }
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(frame.shape, TopAbs_FACE, faces);
        for (int j = 1; j <= faces.Extent(); ++j) {
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(faces(j)), location);
            require(!mesh.IsNull() && mesh->NbTriangles() > 0, "Unmeshed press/tool face");
        }
    }
    require(std::abs(lastFixedY - firstFixedY) > 0.1,
            "One flange incorrectly remained stationary in machine coordinates");
    require(!motion->pressState(61) &&
                motion->pressState(8)->phase == simulation::PressPhase::Bending &&
                std::abs(motion->pressState(40)->section.angleRadians - bend.angleRadians) < 1e-8,
            "Wrong press phase/endpoint metadata");
    require(motion->punchRetraction() && motion->punchRetraction()->liftMm > 0 &&
                motion->punchRetraction()->finalClearanceMm > 1e-5 &&
                motion->pressState(60)->phase == simulation::PressPhase::Retracting &&
                std::abs(motion->pressState(60)->punchTravelMm) < 1e-7,
            "Missing verified punch return or wrong final height");
    auto invalid = tools;
    invalid.punchAngleRadians = 100 * std::numbers::pi / 180;
    const auto rejected = cad::SingleBendPreviewBuilder{}.buildToolMotion(
        document, 0, {bend.angleRadians, bend.innerRadiusMm}, invalid, {});
    require(std::holds_alternative<application::BendPreviewError>(rejected) &&
                std::get<application::BendPreviewError>(rejected).code ==
                    application::BendPreviewErrorCode::ToolAngleConflict,
            "Incompatible tool motion accepted or reported incorrectly");
    std::stop_source cancelled;
    cancelled.request_stop();
    require(
        std::get<application::BendPreviewError>(
            cad::SingleBendPreviewBuilder{}.buildToolMotion(
                document, 0, {bend.angleRadians, bend.innerRadiusMm}, tools, cancelled.get_token()))
                .code == application::BendPreviewErrorCode::Cancelled,
        "Cancelled press motion was built");
    return motion;
}
void checkReturnSweep(const application::BendPreviewPtr &result) {
    const auto &formed = cad::OcctPreviewAccess::frame(*result, 40);
    const auto &returned = cad::OcctPreviewAccess::frame(*result, 60);
    const auto &sweep = cad::OcctPreviewAccess::retractionSweep(*result);
    const auto &report = *result->punchRetraction();
    const auto &tools = result->toolSetup()->tools;
    const auto commonVolume = [](const TopoDS_Shape &a, const TopoDS_Shape &b) {
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape arguments, cutters;
        arguments.Append(a);
        cutters.Append(b);
        common.SetArguments(arguments);
        common.SetTools(cutters);
        common.SetNonDestructive(true);
        common.Build();
        require(common.IsDone() && !common.HasErrors(), "Return test Boolean failed");
        return volume(common.Shape());
    };
    require(!sweep.IsNull() && BRepCheck_Analyzer(sweep).IsValid(),
            "Invalid continuous return volume");
    Bnd_Box box;
    BRepBndLib::AddOptimal(formed.tools[1], box, false, false);
    const double halfWidth = box.CornerMax().X();
    const double addedVolume = 2 * halfWidth * report.liftMm * tools.lengthMm;
    require(std::abs(volume(sweep) - volume(formed.tools[1]) - addedVolume) < 1e-4,
            "Return envelope is not the exact translational sweep");
    for (double fraction : {0.0, 0.137, 0.382, 0.5, 0.749, 1.0}) {
        gp_Trsf lift;
        lift.SetTranslation(gp_Vec(0, fraction * report.liftMm, 0));
        const auto at = BRepBuilderAPI_Transform(formed.tools[1], lift, true).Shape();
        const double covered = commonVolume(at, sweep);
        require(std::abs(covered - volume(at)) < 1e-4,
                "Return sweep misses an intermediate tool pose");
    }
    double clearance = 1e100;
    for (const auto &region : returned.regions) {
        require(commonVolume(sweep, region) < 1e-5, "Swept return intersects the formed sheet");
        BRepExtrema_DistShapeShape distance(region, returned.tools[1]);
        require(distance.IsDone(), "Final clearance test failed");
        clearance = std::min(clearance, distance.Value());
    }
    require(std::abs(clearance - report.finalClearanceMm) < 1e-6 &&
                commonVolume(sweep, formed.tools[0]) < 1e-5,
            "Wrong final clearance or return/die intersection");
    // This obstacle misses both endpoints, but is crossed by the upper side of
    // the translating wedge. Endpoint-only checks must not certify the return.
    const double tangent = std::tan(tools.punchAngleRadians / 2);
    const double x = halfWidth - tangent * report.liftMm / 8;
    const double y = box.CornerMax().Y() + report.liftMm / 2;
    const double epsilon = std::min(0.1, tangent * report.liftMm / 100);
    const auto obstacle = BRepPrimAPI_MakeBox(gp_Pnt(x - epsilon, y - epsilon, tools.lengthMm / 2),
                                              2 * epsilon, 2 * epsilon, 1)
                              .Shape();
    require(commonVolume(obstacle, formed.tools[1]) < 1e-6 &&
                commonVolume(obstacle, returned.tools[1]) < 1e-6 &&
                commonVolume(obstacle, sweep) > epsilon * epsilon,
            "Return sweep missed a collision between clear endpoints");
}
void test(const std::filesystem::path &path) {
    const auto right = load(fixtures::bentStrip(), path);
    const core::PressTools tools{48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
    checkMotion(right, tools);
    auto acuteTools = tools;
    acuteTools.openingMm = 24;
    acuteTools.dieAngleRadians = std::numbers::pi / 3;
    acuteTools.punchAngleRadians = 55 * std::numbers::pi / 180;
    checkMotion(load(fixtures::bentStrip(4, 2, 120 * std::numbers::pi / 180), path), acuteTools);
    const auto buildSetup = [&](core::PressTools parameters) {
        return cad::SingleBendPreviewBuilder{}.buildToolSetup(right, 0, {std::numbers::pi / 2, 4},
                                                              parameters, {});
    };
    for (const double dieDegrees : {30.0, 90.0, 120.0}) {
        auto parameters = tools;
        parameters.dieAngleRadians = dieDegrees * std::numbers::pi / 180;
        const auto result = buildSetup(parameters);
        if (const auto *failure = std::get_if<application::BendPreviewError>(&result)) {
            throw std::runtime_error("Tool setup failed: " + failure->detail);
        }
        const auto setup = std::get<application::BendPreviewPtr>(result);
        require(setup->frameCount() == 1 && setup->toolSetup(), "Setup missing static metadata");
        const auto &frame = cad::OcctPreviewAccess::frame(*setup, 0);
        double materialVolume = 0;
        for (const auto &region : frame.regions) {
            materialVolume += volume(region);
            BRepExtrema_DistShapeShape distance(region, frame.tools[1]);
            require(distance.IsDone() && distance.Value() >= 10 - 1e-5,
                    "Punch penetrates clearance");
        }
        require(std::abs(materialVolume - right->metadata().volumeMm3) < 1e-5,
                "Setup changed material volume");
        BRepExtrema_DistShapeShape leftContact(frame.regions[0], frame.tools[0]);
        BRepExtrema_DistShapeShape rightContact(frame.regions[2], frame.tools[0]);
        require(leftContact.IsDone() && rightContact.IsDone() && leftContact.Value() < 1e-5 &&
                    rightContact.Value() < 1e-5,
                "Flat flanges do not contact die shoulders");
        for (const auto &tool : frame.tools) {
            require(BRepCheck_Analyzer(tool).IsValid() && volume(tool) > 0, "Invalid setup tool");
        }
        Bnd_Box bounds;
        BRepBndLib::Add(frame.tools[0], bounds, false);
        double x0, y0, z0, x1, y1, z1;
        bounds.Get(x0, y0, z0, x1, y1, z1);
        require(std::abs(x0 + 48) < 1e-5 && std::abs(x1 - 48) < 1e-5 && std::abs(y1) < 1e-5 &&
                    std::abs(z0) < 1e-5 && std::abs(z1 - 100) < 1e-5,
                "Incorrect tool machine coordinates");
        const double depth = 24 / std::tan(parameters.dieAngleRadians / 2);
        const double expectedDieVolume = (96 * (depth + 24) - 48 * depth / 2) * 100;
        require(std::abs(volume(frame.tools[0]) - expectedDieVolume) < 1e-4,
                "Incorrect V profile volume");
    }
    for (double angle : {10.0, 120.0}) {
        auto parameters = tools;
        parameters.punchAngleRadians = angle * std::numbers::pi / 180.0;
        parameters.noseRadiusMm = 12;
        require(std::holds_alternative<application::BendPreviewPtr>(buildSetup(parameters)),
                "Boundary punch profile failed");
    }
    auto small = tools;
    small.openingMm = 1;
    small.noseRadiusMm = 0.1;
    small.clearanceMm = 0.1;
    require(std::holds_alternative<application::BendPreviewPtr>(buildSetup(small)),
            "Small profile or clearance failed");
    auto badTools = tools;
    badTools.lengthMm = 79;
    require(std::get<application::BendPreviewError>(buildSetup(badTools)).code ==
                application::BendPreviewErrorCode::ToolTooShort,
            "Short tool was rendered as a checked setup");
    badTools = tools;
    badTools.openingMm = 80;
    require(std::get<application::BendPreviewError>(buildSetup(badTools)).code ==
                application::BendPreviewErrorCode::MissingLeftSupport,
            "Unsupported blank was rendered as a checked setup");
    for (bool fixedSecond : {false, true}) {
        const auto result = preview(right, fixedSecond);
        check(result, right->metadata().volumeMm3);
        require(std::abs(result->model().fixedFlangeMm - 30) < 1.0e-5 &&
                    std::abs(result->model().movingFlangeMm - 30) < 1.0e-5,
                "Wrong reconstructed flange lengths");
        const auto &first = cad::OcctPreviewAccess::frame(*result, 0).regions[0];
        const auto &last = cad::OcctPreviewAccess::frame(*result, 20).regions[0];
        GProp_GProps a, b;
        BRepGProp::VolumeProperties(first, a);
        BRepGProp::VolumeProperties(last, b);
        require(a.CentreOfMass().Distance(b.CentreOfMass()) < 1.0e-7, "Fixed flange moved");
    }
    for (double degrees : {5.0, 30.0, 60.0, 120.0, 150.0, 175.0}) {
        const auto document =
            load(fixtures::bentStrip(4, 2, degrees * std::numbers::pi / 180), path);
        check(preview(document, false), document->metadata().volumeMm3);
    }
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(3, -8, 11), gp_Dir(1, 2, 3)), 1.1);
    const auto rotated =
        load(BRepBuilderAPI_Transform(fixtures::bentStrip(), rotation, true), path, "INCH");
    check(preview(rotated, true), rotated->metadata().volumeMm3);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    const auto mirrored = load(BRepBuilderAPI_Transform(fixtures::bentStrip(), mirror, true), path);
    check(preview(mirrored, false), mirrored->metadata().volumeMm3);
    const auto smallDocument =
        load(fixtures::bentStrip(0.1, 0.1, 5.0 * std::numbers::pi / 180.0, 1), path);
    check(preview(smallDocument, false), smallDocument->metadata().volumeMm3);
    const auto excessive = load(fixtures::bentStrip(4, 2, 178.0 * std::numbers::pi / 180.0), path);
    const auto &excessiveBend = excessive->metadata().bendAnalysis.bends.at(0);
    require(
        std::get<application::BendPreviewError>(
            cad::SingleBendPreviewBuilder{}.build(
                excessive, 0, {excessiveBend.angleRadians, excessiveBend.innerRadiusMm}, false, {}))
                .code == application::BendPreviewErrorCode::UnsupportedParameters,
        "Unsupported angular range lacked a parameter diagnostic");
    const auto reject = [&](application::DocumentPtr document) {
        const auto result = cad::SingleBendPreviewBuilder{}.build(
            document, 0, {std::numbers::pi / 2, 4}, false, {});
        require(std::holds_alternative<application::BendPreviewError>(result) &&
                    std::get<application::BendPreviewError>(result).code ==
                        application::BendPreviewErrorCode::UnsupportedGeometry,
                "Unsupported shape was animated");
    };
    reject(load(fixtures::channel(), path));
    reject(load(fixtures::trimmedBend(), path));
    for (const auto &shape : {fixtures::bendWithZoneOpening(), fixtures::bendWithZoneOpening(false),
                              fixtures::channelWithZoneOpening()}) {
        const auto interrupted = load(shape, path);
        const auto &candidate =
            interrupted->metadata().bendAnalysis.interiorContourDiagnostics.at(0).candidate;
        const core::BendTarget target{candidate.angleRadians, candidate.innerRadiusMm};
        const cad::SingleBendPreviewBuilder builder;
        for (const auto &result : {builder.build(interrupted, 0, target, false, {}),
                                   builder.buildToolSetup(interrupted, 0, target, tools, {}),
                                   builder.buildToolMotion(interrupted, 0, target, tools, {})}) {
            require(std::get<application::BendPreviewError>(result).code ==
                        application::BendPreviewErrorCode::BendZoneInteriorContours,
                    "Interrupted bend was animated or lost its diagnostic reason");
        }
    }
    reject(load(BRepPrimAPI_MakePrism(
                    BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(), gp_Dir(0, 0, 1)), 0, 20, 0, 20).Face(),
                    gp_Vec(0, 0, 2))
                    .Shape(),
                path));
    const auto halfspace =
        BRepPrimAPI_MakeHalfSpace(
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, -20, 0), gp_Dir(0, 1, -0.1))).Face(),
            gp_Pnt(0, -40, 0))
            .Solid();
    const auto oblique = load(BRepAlgoAPI_Cut(fixtures::bentStrip(), halfspace), path);
    require(oblique->metadata().faceCount == 10 &&
                oblique->metadata().bendAnalysis.bends.size() == 1,
            "Negative fixture does not exercise reconstruction proof");
    // A nonrectangular free edge is now retained, rather than replaced by an area/W strip.
    check(preview(oblique, false), oblique->metadata().volumeMm3);
    check(preview(oblique, true), oblique->metadata().volumeMm3);
    const auto boss = load(
        BRepAlgoAPI_Fuse(fixtures::bentStrip(), BRepPrimAPI_MakeBox(gp_Pnt(5.5, -20, 30), 2, 5, 5))
            .Shape(),
        path);
    require(boss->metadata().bendAnalysis.bends.size() == 1, "Boss fixture lost its local bend");
    reject(boss);
    const auto straightCut =
        BRepPrimAPI_MakeHalfSpace(
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, -20, 0), gp_Dir(0, 1, 0))).Face(),
            gp_Pnt(0, -40, 0))
            .Solid();
    const auto asymmetric = load(BRepAlgoAPI_Cut(fixtures::bentStrip(), straightCut), path);
    const auto fixedA = preview(asymmetric, false);
    const auto fixedB = preview(asymmetric, true);
    require(std::abs(fixedA->model().fixedFlangeMm - fixedB->model().movingFlangeMm) < 1.0e-5 &&
                std::abs(fixedA->model().movingFlangeMm - fixedB->model().fixedFlangeMm) < 1.0e-5 &&
                std::abs(fixedA->model().fixedFlangeMm + fixedA->model().movingFlangeMm - 50) <
                    1.0e-5,
            "Fixed-side selection lost unequal flange lengths");
    check(fixedA, asymmetric->metadata().volumeMm3);
    check(fixedB, asymmetric->metadata().volumeMm3);
    auto asymmetricTools = tools;
    asymmetricTools.openingMm = 32;
    checkMotion(asymmetric, asymmetricTools);
    const auto mismatch = cad::SingleBendPreviewBuilder{}.build(right, 0, {1.0, 4}, false, {});
    require(std::get<application::BendPreviewError>(mismatch).code ==
                application::BendPreviewErrorCode::TargetMismatch,
            "Different target silently changed the blank");
    std::stop_source stopped;
    stopped.request_stop();
    require(std::get<application::BendPreviewError>(
                cad::SingleBendPreviewBuilder{}.build(right, 0, {std::numbers::pi / 2, 4}, false,
                                                      stopped.get_token()))
                    .code == application::BendPreviewErrorCode::Cancelled,
            "Cancelled preview published geometry");
}
void testProfiled(const std::filesystem::path &path) {
    const auto document = load(fixtures::profiledBend(), path);
    require(document->metadata().faceCount > 10 &&
                document->metadata().bendAnalysis.bends.size() == 1,
            "Profiled fixture must retain one full bend and detailed flanges");
    const auto &source =
        cad::OcctDocumentAccess::shape(dynamic_cast<const cad::CadDocument &>(*document));
    TopTools_IndexedMapOfShape sourceFaces;
    TopExp::MapShapes(source, TopAbs_FACE, sourceFaces);
    std::vector<Handle(Poly_Triangulation)> meshes;
    for (int i = 1; i <= sourceFaces.Extent(); ++i) {
        TopLoc_Location location;
        meshes.push_back(BRep_Tool::Triangulation(TopoDS::Face(sourceFaces(i)), location));
    }
    for (bool second : {false, true}) {
        const auto result = preview(document, second);
        check(result, document->metadata().volumeMm3);
        require(std::abs(result->model().fixedFlangeMm - 30) < 1e-5 &&
                    std::abs(result->model().movingFlangeMm - 30) < 1e-5,
                "Perforation changed flange envelope lengths");
        for (std::size_t i = 0; i < result->frameCount(); ++i) {
            const auto &frame = cad::OcctPreviewAccess::frame(*result, i);
            TopTools_IndexedMapOfShape frameFaces;
            TopExp::MapShapes(frame.shape, TopAbs_FACE, frameFaces);
            int cones = 0, holes = 0;
            for (int j = 1; j <= frameFaces.Extent(); ++j) {
                require(!sourceFaces.Contains(frameFaces(j)), "Frame shares mutable source faces");
                const BRepAdaptor_Surface surface(TopoDS::Face(frameFaces(j)));
                cones += surface.GetType() == GeomAbs_Cone ? 1 : 0;
                if (surface.GetType() == GeomAbs_Cylinder &&
                    (std::abs(surface.Cylinder().Radius() - 1.5) < 1e-5 ||
                     std::abs(surface.Cylinder().Radius() - 2.5) < 1e-5)) {
                    ++holes;
                }
            }
            require(cones >= 1 && holes >= 2, "A frame filled a hole or lost its countersink");
        }
        const auto &last = cad::OcctPreviewAccess::frame(*result, 20).shape;
        for (bool reverse : {false, true}) {
            BRepAlgoAPI_Cut residual;
            TopTools_ListOfShape arguments, tools;
            arguments.Append(reverse ? last : source);
            tools.Append(reverse ? source : last);
            residual.SetArguments(arguments);
            residual.SetTools(tools);
            residual.SetNonDestructive(true);
            residual.Build();
            require(residual.IsDone() && !residual.HasErrors() && volume(residual.Shape()) < 1e-4,
                    "Profiled endpoint does not match the original STEP");
        }
    }
    const core::PressTools tools{48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
    checkReturnSweep(checkMotion(document, tools));
    for (int i = 1; i <= sourceFaces.Extent(); ++i) {
        TopLoc_Location location;
        require(BRep_Tool::Triangulation(TopoDS::Face(sourceFaces(i)), location) ==
                    meshes[static_cast<std::size_t>(i - 1)],
                "Preview changed the source triangulation");
    }
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(3, -8, 11), gp_Dir(1, 2, 3)), 1.1);
    const auto rotated =
        load(BRepBuilderAPI_Transform(fixtures::profiledBend(), rotation, true), path, "INCH");
    check(preview(rotated, true), rotated->metadata().volumeMm3);
    checkMotion(rotated, tools);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(), gp_Dir(1, 0, 0)));
    const auto mirrored =
        load(BRepBuilderAPI_Transform(fixtures::profiledBend(), mirror, true), path);
    check(preview(mirrored, false), mirrored->metadata().volumeMm3);
    const auto interrupted =
        load(BRepAlgoAPI_Cut(
                 fixtures::bentStrip(8, 6),
                 BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(7, -12, 40), gp_Dir(1, 0, 0)), 2, 8)),
             path);
    check(preview(interrupted, false), interrupted->metadata().volumeMm3);
    const auto &bend = interrupted->metadata().bendAnalysis.bends.at(0);
    for (bool motion : {false, true}) {
        const auto rejected =
            motion ? cad::SingleBendPreviewBuilder{}.buildToolMotion(
                         interrupted, 0, {bend.angleRadians, bend.innerRadiusMm}, tools, {})
                   : cad::SingleBendPreviewBuilder{}.buildToolSetup(
                         interrupted, 0, {bend.angleRadians, bend.innerRadiusMm}, tools, {});
        require(std::get<application::BendPreviewError>(rejected).code ==
                    application::BendPreviewErrorCode::SupportNotProven,
                "A flange envelope was mistaken for intact support material");
    }
    if (const auto *external = std::getenv("BENDING_PROFILED_STEP")) {
        const auto imported = cad::StepImporter{}.load(external, {}, {});
        const auto local = application::resultDocument(imported);
        require(local != nullptr, "Cannot load local profiled STEP");
        check(preview(local, false), local->metadata().volumeMm3);
        check(preview(local, true), local->metadata().volumeMm3);
        checkMotion(local, tools);
        std::cout << "Private profiled STEP: both fixed sides and 61 tool states passed\n";
    }
}
} // namespace
int main() {
    try {
        STEPControl_Controller::Init();
        Temporary temporary;
        test(temporary.path / "part.step");
        testProfiled(temporary.path / "profiled.step");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Bend preview: reconstruction, states, orientation, units and refusals passed\n";
}
