#include "BendFixtures.h"
#include "ProfileFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
application::DocumentPtr load(const TopoDS_Shape &shape, const std::filesystem::path &file) {
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(file.string().c_str()) == IFSelect_RetDone,
            "STEP fixture failed");
    auto d = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
    require(bool(d), "Import failed");
    return d;
}
void visual(const cad::SheetOrientationVisual &v, const TopoDS_Shape &shape, std::size_t edges) {
    require(!v.sides[0].empty() && !v.sides[1].empty(), "A/B skin missing");
    require(v.edges.size() == edges && edges > 0, "Reference edge lost");
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    std::set<int> seen;
    for (const auto &side : v.sides)
        for (const auto &face : side) {
            const auto index = faces.FindIndex(face);
            require(index > 0 && seen.insert(index).second, "Side is not a unique displayed face");
        }
    const auto &edge = v.edges.front();
    const auto reference = gp_Pnt((edge.points[0].XYZ() + edge.points[1].XYZ()) * .5);
    bool attachedToA = false;
    for (const auto &value : v.sides[0]) {
        const auto face = TopoDS::Face(value);
        TopLoc_Location location;
        const auto mesh = BRep_Tool::Triangulation(face, location);
        if (mesh.IsNull())
            continue;
        for (int i = 1; i <= mesh->NbTriangles() && !attachedToA; ++i) {
            int a, b, c;
            mesh->Triangle(i).Get(a, b, c);
            const auto p = mesh->Node(a).Transformed(location.Transformation());
            const gp_Vec u(p, mesh->Node(b).Transformed(location.Transformation())),
                w(p, mesh->Node(c).Transformed(location.Transformation())), q(p, reference);
            auto n = u.Crossed(w);
            if (face.Orientation() == TopAbs_REVERSED)
                n = -n;
            if (n.SquareMagnitude() < 1e-16 || n.Normalized().Dot(edge.normal) < .99 ||
                std::abs(n.Dot(q)) / n.Magnitude() > 1e-4)
                continue;
            const double uu = u.Dot(u), ww = w.Dot(w), uw = u.Dot(w),
                         denominator = uu * ww - uw * uw;
            if (denominator <= 1e-16)
                continue;
            const double x = (ww * q.Dot(u) - uw * q.Dot(w)) / denominator,
                         y = (uu * q.Dot(w) - uw * q.Dot(u)) / denominator;
            attachedToA = x >= -1e-5 && y >= -1e-5 && x + y <= 1 + 1e-5;
        }
    }
    require(attachedToA, "A/B identity flipped relative to the original reference edge");
}
void check(application::DocumentPtr d, bool route) {
    const auto &native = dynamic_cast<const cad::CadDocument &>(*d);
    const auto &source = cad::OcctDocumentAccess::sheetOrientation(native);
    const auto &bends = d->metadata().bendAnalysis.bends;
    require(source.available && !source.reference.edges.empty(), "Source orientation unavailable");
    for (const auto &b : bends) {
        require(source.faces[b.innerFace] != cad::SheetSide::Neutral &&
                    source.faces[b.innerFace] != source.faces[b.outerFace],
                "Cylinder sides conflict");
        for (std::size_t i : {0u, 2u})
            require(source.faces[b.innerFace] == source.faces[b.flangeFaces[i]] &&
                        source.faces[b.outerFace] == source.faces[b.flangeFaces[i + 1]],
                    "Skin continuity broken");
    }
    const auto count = source.reference.edges.size();
    cad::FlatPatternBuilder flatBuilder;
    const auto flatResult =
        flatBuilder.build(d, {{core::SheetMaterial::MildSteel, .45, 1}, {}}, {});
    if (const auto *e = std::get_if<application::FlatPatternError>(&flatResult))
        throw std::runtime_error(e->detail);
    const auto flat = std::get<application::FlatPatternPtr>(flatResult);
    visual(cad::OcctFlatPatternAccess::orientation(*flat), cad::OcctFlatPatternAccess::shape(*flat),
           count);
    cad::SingleBendPreviewBuilder builder;
    application::RouteDraftRequest request;
    for (std::size_t i = 0; i < bends.size(); ++i)
        request.operations.push_back(
            {"B" + std::to_string(i), i, {bends[i].angleRadians, bends[i].innerRadiusMm}, {}, {}});
    for (std::size_t step : {std::size_t(0), bends.size()}) {
        request.step = step;
        const auto result = builder.buildRouteDraft(d, request, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error(e->detail);
        const auto draft = std::get<application::RouteDraftPtr>(result);
        for (std::size_t i = 0; i < draft->visual->frameCount(); ++i) {
            const auto &f = cad::OcctPreviewAccess::frame(*draft->visual, i);
            visual(f.orientation, f.displayShape.IsNull() ? f.shape : f.displayShape, count);
        }
    }
    if (route) {
        application::BendRouteRequest r;
        r.operations = request.operations;
        r.automatic = true;
        const auto result = builder.buildRoute(d, r, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error(e->detail);
        for (const auto &step : std::get<application::BendRoutePtr>(result)->steps)
            for (std::size_t i = 0; i < step.preview->frameCount(); ++i) {
                const auto &f = cad::OcctPreviewAccess::frame(*step.preview, i);
                visual(f.orientation, f.displayShape.IsNull() ? f.shape : f.displayShape, count);
            }
    }
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("sheet-sides-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const auto disk = load(BRepPrimAPI_MakeCylinder(40, 2).Shape(), folder / "disk.step");
        const auto &diskSides = cad::OcctDocumentAccess::sheetOrientation(
            dynamic_cast<const cad::CadDocument &>(*disk));
        require(diskSides.available && diskSides.reference.edges.empty(),
                "A circular outline disabled A/B or invented a straight reference edge");
        check(load(fixtures::channel(), folder / "channel.step"), true);
        check(load(fixtures::tray(), folder / "tray.step"), false);
        auto profile = fixtures::foldedProfile(8, 83, .43, false, true);
        auto document = load(profile, folder / "reverse.step");
        const auto &sides = cad::OcctDocumentAccess::sheetOrientation(
            dynamic_cast<const cad::CadDocument &>(*document));
        std::set<cad::SheetSide> inner;
        for (const auto &b : document->metadata().bendAnalysis.bends)
            inner.insert(sides.faces[b.innerFace]);
        require(inner.size() == 2, "Reverse bends were incorrectly called one inside side");
        check(document, false);
        gp_Trsf moved;
        moved.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .63);
        moved.SetTranslationPart(gp_Vec(40, -25, 17));
        check(load(BRepBuilderAPI_Transform(profile, moved, true).Shape(), folder / "moved.step"),
              false);
        check(load(fixtures::profiledBend(), folder / "holes.step"), true);
        std::filesystem::remove_all(folder);
        std::cout << "Sheet sides, reverse bends, flat, draft and full route passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
