#include "BendFixtures.h"
#include "bending/cad/StepImporter.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>

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
void requireNear(double a, double b, double tolerance, const char *message) {
    require(std::isfinite(a) && std::abs(a - b) <= tolerance, message);
}
struct Temporary {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("bending-zones-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() {
        std::filesystem::create_directory(path);
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
std::unique_ptr<const cad::CadDocument> read(const std::filesystem::path &path) {
    auto result = cad::StepImporter{}.read(path);
    require(std::holds_alternative<std::unique_ptr<const cad::CadDocument>>(result),
            "STEP import failed");
    return std::move(std::get<std::unique_ptr<const cad::CadDocument>>(result));
}
void write(const std::filesystem::path &path, const TopoDS_Shape &shape, const char *unit) {
    require(BRepCheck_Analyzer(shape).IsValid(), "Invalid recognition fixture");
    Interface_Static::SetCVal("write.step.unit", unit);
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone,
            "Fixture transfer failed");
    require(writer.Write(path.string().c_str()) == IFSelect_RetDone, "Fixture write failed");
}
void test(const std::filesystem::path &directory) {
    const auto check = [&](const TopoDS_Shape &shape, const char *name, std::size_t expected,
                           double angle = std::numbers::pi / 2.0, const char *unit = "MM",
                           bool trimmed = false) {
        const auto path = directory / name;
        write(path, shape, unit);
        const auto document = read(path);
        const auto &result = document->metadata().bendAnalysis;
        require(result.status == application::BendRecognitionStatus::Completed,
                "Recognition incomplete");
        std::cout << name << ": " << result.bends.size() << " bends, "
                  << result.unrecognizedCylindricalFaces << " unmatched cylinders\n";
        require(result.bends.size() == expected, "Unexpected bend count");
        require(result.interiorContourDiagnostics.empty(),
                "Plain geometry produced a false opening diagnostic");
        for (const auto &bend : result.bends) {
            requireNear(bend.angleRadians, angle, 1.0e-6, "Wrong bend angle");
            requireNear(bend.innerRadiusMm, 4.0, 1.0e-5, "Wrong inner radius");
            requireNear(bend.thicknessMm, 2.0, 1.0e-5, "Wrong bend thickness");
            requireNear(bend.lengthMm, 80.0, 1.0e-5, "Wrong bend length");
            require(bend.trimmedEnds == trimmed, "Wrong trimmed-end classification");
            requireNear(bend.minimumStraightLengthMm, trimmed ? 74.0 : 80.0, 1.0e-4,
                        "Wrong common straight span");
            requireNear(std::hypot(bend.axisEnd.x - bend.axisStart.x,
                                   bend.axisEnd.y - bend.axisStart.y,
                                   bend.axisEnd.z - bend.axisStart.z),
                        80.0, 1.0e-5, "Wrong axis endpoints");
            for (const auto i : bend.flangeFaces) {
                require(i > 0 && i <= document->faces().size(), "Bad flange reference");
                require(document->faces()[i - 1].surface == application::SurfaceKind::Plane,
                        "Flange is not planar");
            }
            for (const auto i : {bend.innerFace, bend.outerFace}) {
                require(i > 0 && i <= document->faces().size(), "Bad cylinder reference");
                require(document->faces()[i - 1].surface == application::SurfaceKind::Cylinder,
                        "Bend is not cylindrical");
            }
        }
    };
    check(fixtures::bentStrip(), "right.step", 1);
    check(fixtures::bentStrip(), "inches.step", 1, std::numbers::pi / 2, "INCH");
    for (double degrees : {30.0, 60.0, 120.0, 150.0}) {
        const auto angle = degrees * std::numbers::pi / 180.0;
        check(fixtures::bentStrip(4, 2, angle), "angle.step", 1, angle);
    }
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(3, -8, 11), gp_Dir(1, 2, 3)), 1.1);
    check(BRepBuilderAPI_Transform(fixtures::bentStrip(), transform, true), "rotated.step", 1);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    check(BRepBuilderAPI_Transform(fixtures::bentStrip(), mirror, true), "mirrored.step", 1);
    check(fixtures::channel(), "channel.step", 2);
    check(fixtures::trimmedBend(), "trimmed.step", 1, std::numbers::pi / 2, "MM", true);
    check(fixtures::trimmedBend(), "trimmed-inch.step", 1, std::numbers::pi / 2, "INCH", true);
    check(BRepBuilderAPI_Transform(fixtures::trimmedBend(), transform, true),
          "trimmed-rotated.step", 1, std::numbers::pi / 2, "MM", true);
    const auto flangeHole =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(2, -15, 40), gp_Dir(1, 0, 0)), 3, 6).Shape();
    check(BRepAlgoAPI_Cut(fixtures::bentStrip(), flangeHole), "flange-hole.step", 1);
    const auto notch = BRepPrimAPI_MakeBox(gp_Pnt(3.5, 3.5, 70), 2, 2, 15).Shape();
    check(BRepAlgoAPI_Cut(fixtures::bentStrip(), notch), "notched-bend.step", 0);
    check(BRepPrimAPI_MakeBox(100, 60, 2), "plate.step", 0);
    check(BRepPrimAPI_MakeCylinder(8, 80), "cylinder.step", 0);
    check(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 80, false), "sector.step", 0);
    const auto hole =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(30, 30, -1), gp_Dir(0, 0, 1)), 5, 4).Shape();
    check(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(100, 60, 2), hole), "hole.step", 0);
    check(fixtures::bentStrip(4, 2, 0.5 * std::numbers::pi / 180), "near-flat.step", 0);
    check(fixtures::bentStrip(4, 2, std::numbers::pi), "hem.step", 0);
    BRepBuilderAPI_MakePolygon polygon;
    for (int i = 0; i < 520; ++i) {
        polygon.Add(fixtures::polar(1000.0, i * 2.0 * std::numbers::pi / 520.0));
    }
    polygon.Close();
    const auto manyFaces =
        BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon.Wire()), gp_Vec(0, 0, 2)).Shape();
    const auto limitedPath = directory / "limited.step";
    write(limitedPath, manyFaces, "MM");
    const auto limited = read(limitedPath);
    require(limited->metadata().bendAnalysis.status ==
                    application::BendRecognitionStatus::LimitReached &&
                limited->metadata().bendAnalysis.bends.empty(),
            "Complexity limit reported a complete analysis");
    std::stop_source stop;
    auto cancelled = cad::StepImporter{}.read(
        limitedPath, stop.get_token(), [&](application::ImportPhase phase) {
            if (phase == application::ImportPhase::Analyzing) {
                stop.request_stop();
            }
        });
    require(std::get<cad::StepImportError>(cancelled).code == cad::StepImportErrorCode::Cancelled,
            "Cancelled analysis published a document");
}
void testInteriorContours(const std::filesystem::path &directory) {
    const auto check = [&](const TopoDS_Shape &shape, const char *unit = "MM") {
        const auto path = directory / "zone-opening.step";
        write(path, shape, unit);
        const auto document = read(path);
        const auto &analysis = document->metadata().bendAnalysis;
        require(analysis.status == application::BendRecognitionStatus::Completed &&
                    analysis.bends.size() == 1 && analysis.interiorContourDiagnostics.size() == 1 &&
                    document->metadata().closedCutouts.status ==
                        application::ClosedCutoutStatus::Filled,
                "Closed cutout approximation or source evidence was lost");
        const auto &diagnostic = analysis.interiorContourDiagnostics.front();
        const auto &candidate = diagnostic.candidate;
        requireNear(candidate.innerRadiusMm, 8, 1e-5, "Wrong diagnostic radius");
        requireNear(candidate.thicknessMm, 6, 1e-5, "Wrong diagnostic cylinder separation");
        requireNear(candidate.angleRadians, std::numbers::pi / 2, 1e-6, "Wrong diagnostic angle");
        requireNear(candidate.lengthMm, 80, 1e-5, "Wrong diagnostic extent");
        require(!diagnostic.boundaryFaces.empty(), "Opening boundary evidence is missing");
        for (const auto face : diagnostic.boundaryFaces) {
            require(face > 0 && face <= document->faces().size() && face != candidate.innerFace &&
                        face != candidate.outerFace,
                    "Invalid opening evidence face");
        }
    };
    check(fixtures::bendWithZoneOpening());
    check(fixtures::bendWithZoneOpening(false));
    constexpr double angle = std::numbers::pi / 6.0;
    const auto secondHole =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(4 * std::cos(angle), 4 * std::sin(angle), 60),
                                        gp_Dir(std::cos(angle), std::sin(angle), 0)),
                                 2, 14)
            .Shape();
    check(BRepAlgoAPI_Cut(fixtures::bendWithZoneOpening(), secondHole).Shape());
    check(fixtures::bendWithZoneOpening(), "INCH");
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(3, -8, 11), gp_Dir(1, 2, 3)), 1.1);
    check(BRepBuilderAPI_Transform(fixtures::bendWithZoneOpening(), transform, true));
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(), gp_Dir(1, 0, 0)));
    check(BRepBuilderAPI_Transform(fixtures::bendWithZoneOpening(), mirror, true));
    const auto sector =
        BRepAlgoAPI_Cut(fixtures::bentStrip(8, 6, std::numbers::pi / 2, 80, false),
                        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(3, 3, 40), gp_Dir(1, 1, 0)), 2, 20))
            .Shape();
    const auto path = directory / "sector-opening.step";
    write(path, sector, "MM");
    require(read(path)->metadata().bendAnalysis.interiorContourDiagnostics.empty(),
            "An isolated round sector was called an interrupted bend");
    write(path, fixtures::channelWithZoneOpening(), "MM");
    const auto mixed = read(path);
    require(mixed->metadata().bendAnalysis.bends.size() == 2 &&
                mixed->metadata().bendAnalysis.interiorContourDiagnostics.size() == 1,
            "An interrupted zone hid the separate complete bend");
}
} // namespace
int main() {
    try {
        STEPControl_Controller::Init();
        Temporary temporary;
        test(temporary.path);
        testInteriorContours(temporary.path);
        if (const auto *path = std::getenv("BENDING_RECOGNITION_STEP")) {
            const auto document = read(path);
            const auto &analysis = document->metadata().bendAnalysis;
            std::cout << "Local model: " << analysis.bends.size() << " bend zones, "
                      << analysis.unrecognizedCylindricalFaces << " unmatched cylindrical faces\n";
            if (const auto *expected = std::getenv("BENDING_RECOGNITION_COUNT")) {
                require(analysis.bends.size() == std::stoul(expected),
                        "Unexpected local-model bend count");
            }
            for (const auto &bend : analysis.bends) {
                std::cout << "  R=" << bend.innerRadiusMm << " t=" << bend.thicknessMm
                          << " angle=" << bend.angleRadians * 180 / std::numbers::pi
                          << " L=" << bend.lengthMm << " trimmed=" << bend.trimmedEnds
                          << " core=" << bend.minimumStraightLengthMm << '\n';
            }
        }
        return 0;
    } catch (const Standard_Failure &error) {
        std::cerr << error.GetMessageString() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
