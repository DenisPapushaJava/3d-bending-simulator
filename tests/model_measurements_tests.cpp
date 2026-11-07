#include "ModelMeasurements.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/simulation/SingleBend.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void close(double a, double b, const char *message) {
    check(std::abs(a - b) < 1.e-6, message);
}
gp_Pnt point(bending::application::PointMm p) {
    return {p.x, p.y, p.z};
}
} // namespace
int main() {
    using namespace bending;
    try {
        auto shape = BRepPrimAPI_MakeBox(100, 80, 3).Shape();
        const auto cylinder = [](double x, double y, double radius) {
            return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, 0), gp_Dir(0, 0, 1)), radius, 3)
                .Shape();
        };
        shape = BRepAlgoAPI_Cut(shape, cylinder(25, 30, 5)).Shape();
        auto slot = BRepPrimAPI_MakeBox(gp_Pnt(59, 41, 0), 12, 8, 3).Shape();
        slot = BRepAlgoAPI_Fuse(slot, cylinder(59, 45, 4)).Shape();
        slot = BRepAlgoAPI_Fuse(slot, cylinder(71, 45, 4)).Shape();
        shape = BRepAlgoAPI_Cut(shape, slot).Shape();
        application::DocumentMetadata metadata{};
        const auto data = cad::prepareModelMeasurements(shape, metadata, {});
        const auto circle = std::find_if(data->openings.begin(), data->openings.end(),
                                         [](const auto &o) { return o.circular; });
        const auto elongated = std::find_if(data->openings.begin(), data->openings.end(),
                                            [](const auto &o) { return !o.circular; });
        check(circle != data->openings.end() && elongated != data->openings.end(),
              "Round hole or supported slot not measured");
        close(circle->lengthMm, 10, "Wrong hole diameter");
        close(elongated->lengthMm, 20, "Slot length is not full outside length");
        close(elongated->widthMm, 8, "Wrong slot width");
        double outlineLength = 0;
        for (std::size_t i = 0; i < elongated->outline.size(); ++i)
            outlineLength +=
                point(elongated->outline[i])
                    .Distance(point(elongated->outline[(i + 1) % elongated->outline.size()]));
        check(std::abs(outlineLength - (24 + 8 * std::numbers::pi)) < .1,
              "Slot selection outline lost wire edge orientation");
        close(circle->center.x, 25, "Wrong circle center");
        close(circle->center.y, 30, "Wrong circle center");
        check(circle->locations.size() == 2 && elongated->locations.size() == 2,
              "Finite base edges not prepared");
        for (const auto &opening : data->openings)
            for (const auto &span : opening.locations)
                close(point(span.first).Distance(point(span.second)), span.valueMm,
                      "Location witness is inconsistent");
        auto moved = cad::transformOpening(*circle, gp_Trsf{});
        close(moved.center.x, circle->center.x, "Identity transformation changed feature");
        std::stop_source stop;
        stop.request_stop();
        check(cad::prepareModelMeasurements(shape, metadata, stop.get_token())->edges.empty(),
              "Cancelled preparation did work");
        const auto started = std::chrono::steady_clock::now();
        auto loaded = cad::StepImporter{}.read(std::filesystem::path(BENDING_EXAMPLE_DIR) /
                                               "bend-90-6mm.step");
        check(std::holds_alternative<std::unique_ptr<const cad::CadDocument>>(loaded),
              "Cannot import test bend");
        const auto &doc = *std::get<std::unique_ptr<const cad::CadDocument>>(loaded);
        const auto measured = cad::OcctDocumentAccess::measurements(doc);
        check(measured && measured->flanges.size() == 2,
              "Source outside flange dimensions not prepared");
        for (const auto &span : measured->flanges) {
            close(span.valueMm, 44, "Source flange is not measured to the outside virtual corner");
            close(point(span.first).Distance(point(span.second)), span.valueMm,
                  "Wrong source 3D witnesses");
        }
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        for (double angle : {5., 30., 55., 90., 120., 175.}) {
            simulation::SingleBend model{6, 8, angle * std::numbers::pi / 180, 80, 30, 40};
            for (double progress : {.01, .3, .8, 1.}) {
                const auto section = *simulation::singleBendSection(model, progress);
                const auto outside = simulation::outsideFlangePoints(section);
                check(outside.has_value(), "Valid formed section lost its outside corner");
                const auto length = [](auto a, auto b) { return std::hypot(a.x - b.x, a.y - b.y); };
                const auto setback = (*section.innerRadiusMm + model.thicknessMm) *
                                     std::tan(section.angleRadians / 2);
                close(length((*outside)[0], (*outside)[1]), model.fixedFlangeMm + setback,
                      "Wrong fixed outside dimension");
                close(length((*outside)[1], (*outside)[2]), model.movingFlangeMm + setback,
                      "Wrong moving outside dimension");
            }
            check(!simulation::outsideFlangePoints(*simulation::singleBendSection(model, 0)),
                  "Flat blank acquired a false virtual corner");
        }
        std::cout << "Model measurements passed; bend import including preparation " << ms
                  << " ms\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
