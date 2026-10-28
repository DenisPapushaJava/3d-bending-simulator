#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
application::DocumentPtr load(const std::filesystem::path &path) {
    const auto d = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(bool(d), "Import failed");
    return d;
}
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("multi-operation-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const bool sample = argc >= 2;
        auto file = sample ? std::filesystem::path(argv[1]) : folder / "tray.step";
        if (!sample) {
            STEPControl_Writer writer;
            require(writer.Transfer(fixtures::tray(), STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Fixture write failed");
        }
        const auto doc = load(file);
        const auto bytes = std::string(doc->sourceStep());
        const auto &bends = doc->metadata().bendAnalysis.bends;
        unsigned successful = 0;
        for (std::size_t i = 0; i < bends.size(); ++i) {
            if (argc == 4 && std::string_view(argv[2]) == "--bend" && i != std::stoul(argv[3]))
                continue;
            const auto &b = bends[i];
            const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
            const auto result = cad::SingleBendPreviewBuilder{}.build(doc, i, target, false, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&result)) {
                std::cout << "B" << i + 1 << " kinematic error: " << error->detail << '\n';
                if (!sample)
                    throw std::runtime_error(error->detail);
                continue;
            }
            const auto preview = std::get<application::BendPreviewPtr>(result);
            ++successful;
            require(preview->otherFormedBends() == bends.size() - 1 && preview->frameCount() == 21,
                    "Whole-part operation scope lost");
            const double expected = volume(cad::OcctPreviewAccess::frame(*preview, 20).shape);
            for (std::size_t f = 0; f < preview->frameCount(); ++f) {
                const auto &frame = cad::OcctPreviewAccess::frame(*preview, f);
                require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                            std::abs(volume(frame.shape) - expected) <
                                std::max(.01, expected * 1.e-7),
                        "Whole-part frame lost material");
                require(!frame.displayShape.IsNull(), "Original holes not displayed");
            }
            if (!sample && i > 0)
                continue; // One press cycle plus every kinematic component split.
            core::PressTools tools{
                8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8, b.lengthMm, 40};
            const auto motion =
                cad::SingleBendPreviewBuilder{}.buildToolMotion(doc, i, target, tools, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&motion))
                std::cout << "B" << i + 1 << " conditional tool L=" << b.lengthMm
                          << " error=" << error->detail
                          << " frame=" << error->frameIndex.value_or(999)
                          << " diagnostic=" << bool(error->diagnostic) << '\n';
            else {
                const auto p = std::get<application::BendPreviewPtr>(motion);
                require(p->frameCount() == 61 && p->punchRetraction() &&
                            p->otherFormedBends() == bends.size() - 1,
                        "Incomplete press motion");
                std::cout << "B" << i + 1 << " conditional tool L=" << b.lengthMm << " motion OK\n";
            }
        }
        require(successful > 0 && doc->sourceStep() == bytes,
                "No supported operation or source changed");
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
