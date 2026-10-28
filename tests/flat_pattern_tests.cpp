#include "BendFixtures.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
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

int main(int argc, char **argv) {
    try {
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
        auto channel = load(fixtures::channel(), folder / "channel.step");
        auto p = build(channel);
        check(p->info().bends.size() == 2 &&
                  std::abs(p->info().bounds.maximum.y -
                           (34 + 80 + 34 + std::numbers::pi * (4 + .43 * 2))) < 1e-4,
              "Wrong U blank");
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
        GProp_GProps props;
        BRepGProp::VolumeProperties(cad::OcctFlatPatternAccess::shape(*holed), props);
        check(std::abs(std::abs(props.Mass()) -
                       (80 * holed->info().bounds.maximum.y * 2 - std::numbers::pi * 4 * 2)) < 1e-3,
              "Flange hole was filled or distorted");
        auto unsupported = load(fixtures::bendWithZoneOpening(), folder / "zone-hole.step");
        auto r = cad::FlatPatternBuilder{}.build(unsupported,
                                                 {{core::SheetMaterial::MildSteel, {}, 1}, {}}, {});
        check(std::holds_alternative<application::FlatPatternError>(r),
              "Zone opening silently filled");
        auto extra = BRepAlgoAPI_Fuse(fixtures::bentStrip(),
                                      BRepPrimAPI_MakeBox(gp_Pnt(4, -20, 30), 5, 5, 5))
                         .Shape();
        r = cad::FlatPatternBuilder{}.build(load(extra, folder / "extra.step"),
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
