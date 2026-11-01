#include "BendFixtures.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace bending;
using namespace bending::application;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
DocumentPtr load(const std::filesystem::path &file) {
    const auto start = std::chrono::steady_clock::now();
    auto result = cad::StepImporter{}.load(file, {}, {});
    auto doc = resultDocument(result);
    if (!doc)
        throw std::runtime_error(std::get<ImportError>(result).detail);
    const auto &t = doc->metadata().thickness;
    std::cout << file.filename().string() << " ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << " faces=" << doc->metadata().faceCount << " status=" << static_cast<int>(t.status)
              << " proposal=" << suggestThickness(t).value_or(-1)
              << " evidence=" << t.evidence.size()
              << " bends=" << doc->metadata().bendAnalysis.bends.size() << std::endl;
    check(!doc->sourceStep().empty(), "Lost original source");
    for (const auto &pair : t.evidence) {
        check(pair.firstFace && pair.secondFace > pair.firstFace &&
                  pair.secondFace <= doc->faces().size(),
              "Invalid evidence face reference");
        const auto kind = pair.kind == ThicknessEvidenceKind::BendRadii ? SurfaceKind::Cylinder
                                                                        : SurfaceKind::Plane;
        check(doc->faces()[pair.firstFace - 1].surface == kind &&
                  doc->faces()[pair.secondFace - 1].surface == kind,
              "Evidence origin does not match actual source surfaces");
    }
    return doc;
}
void proposal(const DocumentPtr &doc, double expected, bool confirmed) {
    const auto &t = doc->metadata().thickness;
    const auto value = suggestThickness(t);
    check(value && std::abs(*value - expected) < 1e-5, "Wrong automatic proposal");
    check((t.status == ThicknessStatus::Detected) == confirmed &&
              t.thicknessMm.has_value() == confirmed,
          "Local evidence became a global proof");
    check(validateThickness(*value, t) ==
              (confirmed ? ThicknessValidation::Verified : ThicknessValidation::LocalMatch),
          "Thickness confidence was lost");
}
TopoDS_Shape facetedPlate(unsigned sides) {
    BRepBuilderAPI_MakePolygon wire;
    for (unsigned i = 0; i < sides; ++i) {
        const double angle = 2 * std::numbers::pi * i / sides;
        wire.Add(gp_Pnt(50 * std::cos(angle), 50 * std::sin(angle), 0));
    }
    wire.Close();
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 2)).Shape();
}
} // namespace
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("thickness-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        if (argc > 1) {
            load(std::filesystem::path(argv[1]));
            return 0;
        }
        std::filesystem::create_directories(folder);
        const auto import = [&](const char *name, const TopoDS_Shape &shape) {
            check(BRepCheck_Analyzer(shape).IsValid(), "Invalid fixture");
            const auto path = folder / name;
            STEPControl_Writer writer;
            check(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                      writer.Write(path.string().c_str()) == IFSelect_RetDone,
                  "Write failed");
            return load(path);
        };
        const auto plate = BRepPrimAPI_MakeBox(100, 60, 2).Shape();
        proposal(import("plate.step", plate), 2, true);
        const auto faceted = import("many-faces.step", facetedPlate(140));
        check(faceted->metadata().faceCount > 128, "Old complexity limit not exercised");
        proposal(faceted, 2, true);
        proposal(import("angle.step", fixtures::bentStrip()), 2, false);
        proposal(import("narrow-channel.step", fixtures::channel(80, 2, 40)), 2, false);
        proposal(import("profiled.step", fixtures::profiledBend()), 6, false);
        proposal(import("zone-opening.step", fixtures::bendWithZoneOpening()), 6, false);
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(120, -50, 30), gp_Dir(1, 2, 3)), .7);
        proposal(import("rotated.step",
                        BRepBuilderAPI_Transform(fixtures::channel(), rotation, true).Shape()),
                 2, false);
        const auto pocket = BRepPrimAPI_MakeBox(gp_Pnt(10, 10, 1), 20, 20, 3).Shape();
        const auto variable = import("variable.step", BRepAlgoAPI_Cut(plate, pocket).Shape());
        check(variable->metadata().thickness.status == ThicknessStatus::Ambiguous &&
                  !suggestThickness(variable->metadata().thickness),
              "A stepped thickness silently picked one value");
        const auto flangePocket = BRepPrimAPI_MakeBox(gp_Pnt(5, -25, 10), 2, 10, 20).Shape();
        const auto pocketedBend = import(
            "pocketed-bend.step", BRepAlgoAPI_Cut(fixtures::bentStrip(), flangePocket).Shape());
        check(pocketedBend->metadata().bendAnalysis.bends.size() == 1 &&
                  pocketedBend->metadata().thickness.status == ThicknessStatus::Ambiguous &&
                  !suggestThickness(pocketedBend->metadata().thickness),
              "Bend-radius consensus hid a real wall-thickness conflict");
        gp_Trsf next;
        next.SetTranslation(gp_Vec(0, 0, 30));
        const auto variableBends = import(
            "different-bends.step",
            BRepAlgoAPI_Fuse(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 30),
                             BRepBuilderAPI_Transform(
                                 fixtures::bentStrip(4, 3, std::numbers::pi / 2, 30), next, true)
                                 .Shape())
                .Shape());
        check(variableBends->metadata().thickness.status == ThicknessStatus::Ambiguous &&
                  !suggestThickness(variableBends->metadata().thickness),
              "Different bend/wall thicknesses were silently averaged");
        check(!suggestThickness(import("block.step", BRepPrimAPI_MakeBox(20, 20, 20).Shape())
                                    ->metadata()
                                    .thickness),
              "A block became sheet metal");
        const auto panel =
            load(std::filesystem::path(BENDING_EXAMPLE_DIR) / "wall-panel-2700mm.step");
        proposal(panel, 1.5, false);
        check(panel->metadata().bendAnalysis.bends.size() == 16 &&
                  std::count_if(panel->metadata().thickness.evidence.begin(),
                                panel->metadata().thickness.evidence.end(),
                                [](const auto &pair) {
                                    return pair.kind == ThicknessEvidenceKind::BendRadii;
                                }) == 16,
              "Lost bend-radius measurements");
        const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(panel);
        proposal(
            import("rotated-panel.step",
                   BRepBuilderAPI_Transform(cad::OcctDocumentAccess::shape(*native), rotation, true)
                       .Shape()),
            1.5, false);
        const auto limited = import("bounded.step", facetedPlate(520));
        check(limited->metadata().thickness.status == ThicknessStatus::LimitReached &&
                  !suggestThickness(limited->metadata().thickness),
              "Incomplete probing supplied an automatic thickness");
        std::filesystem::remove_all(folder);
        std::cout << "Plate proof, local proposals, holes, air gaps, rotation, variation and "
                     "limits passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(folder);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
