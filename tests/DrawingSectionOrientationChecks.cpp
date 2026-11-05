#include "DrawingSectionOrientationChecks.h"
#include "BendFixtures.h"
#include "DrawingProfiles.h"
#include "DrawingSections.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <QString>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <stdexcept>

namespace {
using namespace bending;
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
gp_Vec vector(application::PointMm p) {
    return {p.x, p.y, p.z};
}
void frames() {
    const gp_Ax2 source(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3), gp_Dir(3, -1, 1));
    const gp_Pnt origin(57, -13, 28);
    for (double angle : {0., .39, .93, std::numbers::pi / 2, 2.7, std::numbers::pi, -.64})
        for (double side : {-1., 1.}) {
            const gp_Dir normal((gp_Vec(source.XDirection()) * std::cos(angle) +
                                 gp_Vec(source.YDirection()) * std::sin(angle)) *
                                side);
            const auto cut = cad::drawingSectionFrame(origin, normal, source);
            const gp_Dir trace(gp_Vec(source.Direction()).Crossed(gp_Vec(normal)));
            gp_Trsf fold;
            fold.SetRotation(gp_Ax1(origin, trace), std::numbers::pi / 2);
            const auto right = source.XDirection().Transformed(fold);
            const auto up = source.YDirection().Transformed(fold);
            require(
                cut.Location().Distance(origin) < 1e-10 && cut.Direction().IsEqual(normal, 1e-10) &&
                    cut.XDirection().IsEqual(right, 1e-10) && cut.YDirection().IsEqual(up, 1e-10),
                "Section gained a roll or reflection relative to its source trace");
            // An asymmetric physical point, projected with an independent OCCT fold.
            const auto p = origin.Translated(gp_Vec(right) * 17 + gp_Vec(up) * 43);
            const gp_Vec delta(origin, p);
            require(std::abs(delta.Dot(gp_Vec(cut.XDirection())) - 17) < 1e-9 &&
                        std::abs(delta.Dot(gp_Vec(cut.YDirection())) - 43) < 1e-9,
                    "A physical point changed side on the section");
        }
    const gp_Ax2 front(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
    const auto fromRight = cad::drawingSectionFrame(origin, gp_Dir(1, 0, 0), front);
    const auto fromLeft = cad::drawingSectionFrame(origin, gp_Dir(-1, 0, 0), front);
    require(fromRight.XDirection().IsEqual(gp_Dir(0, 0, -1), 1e-10) &&
                fromLeft.XDirection().IsEqual(gp_Dir(0, 0, 1), 1e-10) &&
                fromRight.YDirection().IsEqual(front.YDirection(), 1e-10) &&
                fromLeft.YDirection().IsEqual(front.YDirection(), 1e-10),
            "Opposite section views did not preserve front-up and reverse the visible side");
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
void material(const QString &path, const TopoDS_Shape &input, const gp_Ax2 &reference,
              const std::vector<gp_Pnt> &corners, bool fromTop = false) {
    STEPControl_Writer writer;
    require(writer.Transfer(input, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
            "Asymmetric section fixture could not be written");
    const auto document =
        application::resultDocument(cad::StepImporter{}.load(native(path), {}, {}));
    require(document && document->metadata().bendAnalysis.bends.size() == 1,
            "Asymmetric section fixture did not retain its bend");
    const auto bytes = std::string(document->sourceStep());
    const auto cadDocument = std::dynamic_pointer_cast<const cad::CadDocument>(document);
    require(cadDocument != nullptr, "Fixture has no CAD document");
    const auto shape =
        BRepBuilderAPI_Copy(cad::OcctDocumentAccess::shape(*cadDocument), true, false).Shape();
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    std::array<gp_Ax2, 3> source{
        reference, gp_Ax2(gp_Pnt(0, 0, 0), reference.YDirection(), reference.XDirection()),
        gp_Ax2(gp_Pnt(0, 0, 0), reference.XDirection().Reversed(), reference.Direction())};
    if (fromTop) {
        source[1] = reference;
        source[0] = gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
    }
    application::BendingDrawing drawing;
    drawing.classicalViews.emplace();
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    for (std::size_t i = 0; i < source.size(); ++i) {
        auto &v = (*drawing.classicalViews)[i];
        v.minimum = {1e100, 1e100};
        v.maximum = {-1e100, -1e100};
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1}) {
                    const gp_Vec p(x, y, z);
                    const double u = p.Dot(gp_Vec(source[i].XDirection()));
                    const double w = p.Dot(gp_Vec(source[i].YDirection()));
                    v.minimum = {std::min(v.minimum.x, u), std::min(v.minimum.y, w)};
                    v.maximum = {std::max(v.maximum.x, u), std::max(v.maximum.y, w)};
                }
    }
    const auto &bends = document->metadata().bendAnalysis.bends;
    drawing.profiles = cad::drawingProfiles(bends, faces, source[0], 1e-5, {});
    std::size_t budget = 0;
    cad::prepareClassicalDrawing(shape, faces, bends, source, drawing, 1e-5, budget, {});
    require(drawing.sections.size() == 1 && drawing.sections[0].basis &&
                !drawing.sections[0].rotated,
            "Asymmetric material section is missing or silently rotated");
    const auto &section = drawing.sections[0];
    const auto &basis = *section.basis;
    const auto &location = section.locations.at(0);
    const auto &base = source[location.sourceView];
    require(basis.sourceView == location.sourceView && location.sourceView == (fromTop ? 1u : 0u),
            "Section used a frame from another source projection");
    const gp_Vec normal = vector(section.normal);
    const gp_Dir trace(gp_Vec(base.Direction()).Crossed(normal));
    gp_Trsf fold;
    fold.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), trace), std::numbers::pi / 2);
    const gp_Vec expectedRight(base.XDirection().Transformed(fold));
    const gp_Vec expectedUp(base.YDirection().Transformed(fold));
    require((vector(basis.right) - expectedRight).Magnitude() < 1e-8 &&
                (vector(basis.up) - expectedUp).Magnitude() < 1e-8 &&
                std::abs(location.marker.viewDirection.x + normal.Dot(gp_Vec(base.XDirection()))) <
                    1e-8 &&
                std::abs(location.marker.viewDirection.y + normal.Dot(gp_Vec(base.YDirection()))) <
                    1e-8,
            "Material, dimensions and arrows disagree on the viewing frame");
    const gp_Pnt origin(basis.origin.x, basis.origin.y, basis.origin.z);
    for (const auto &corner : corners) {
        // The known free corners extend along the bend axis; project at this cut.
        const gp_Vec delta(origin, corner);
        const application::DrawingPoint expected{delta.Dot(expectedRight), delta.Dot(expectedUp)};
        bool found = false;
        for (const auto &path : section.view.paths)
            for (const auto &p : path.points)
                found = found || std::hypot(p.x - expected.x, p.y - expected.y) < 1e-4;
        require(found, "A known asymmetric CAD corner is on the wrong side of the section");
    }
    for (double length : {18., 36.})
        require(std::any_of(section.view.dimensions.begin(), section.view.dimensions.end(),
                            [&](const auto &d) { return std::abs(d.valueMm - length) < 1e-4; }),
                "Frame orientation changed an outside flange dimension");
    require(section.view.angles.size() == 1 &&
                std::abs(section.view.angles[0].radians - std::numbers::pi / 2) < 1e-7 &&
                document->sourceStep() == bytes,
            "Section orientation changed the true angle or source STEP");
}
} // namespace
void drawingSectionOrientationChecks(const QString &folder) {
    frames();
    const auto shape = BRepAlgoAPI_Cut(bending::fixtures::bentStrip(),
                                       BRepPrimAPI_MakeBox(gp_Pnt(-40, -5, -1), 28, 20, 82).Shape())
                           .Shape();
    const std::vector<gp_Pnt> corners{{6, -30, 40}, {4, -30, 40}, {-12, 4, 40}, {-12, 6, 40}};
    const gp_Ax2 front(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 0, 1));
    material(folder + "/section-upright.step", shape, front, corners);
    material(folder + "/section-oblique.step", shape,
             gp_Ax2(gp_Pnt(0, 0, 0), front.Direction(), gp_Dir(0, std::sin(.43), std::cos(.43))),
             corners);
    material(folder + "/section-top-source.step", shape, front, corners, true);
    gp_Trsf move;
    move.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .73);
    move.SetTranslationPart(gp_Vec(113, -27, 81));
    std::vector<gp_Pnt> moved;
    for (const auto &p : corners)
        moved.push_back(p.Transformed(move));
    material(folder + "/section-moved.step", BRepBuilderAPI_Transform(shape, move, true).Shape(),
             gp_Ax2(gp_Pnt(0, 0, 0), front.Direction().Transformed(move),
                    front.XDirection().Transformed(move)),
             moved);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    moved.clear();
    for (const auto &p : corners)
        moved.push_back(p.Transformed(mirror));
    material(folder + "/section-mirrored.step",
             BRepBuilderAPI_Transform(shape, mirror, true).Shape(), front, moved);
}
