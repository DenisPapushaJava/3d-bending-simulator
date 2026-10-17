#include "BendFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <chrono>
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
void test(const std::filesystem::path &path) {
    const auto right = load(fixtures::bentStrip(), path);
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
    reject(oblique);
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
} // namespace
int main() {
    try {
        STEPControl_Controller::Init();
        Temporary temporary;
        test(temporary.path / "part.step");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Bend preview: reconstruction, states, orientation, units and refusals passed\n";
}
