#include "CollisionHighlight.h"
#include "CollisionStrip.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace bending::cad;
namespace {
void require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
struct Fixture {
    TopoDS_Shape sheet, punch, overlap;
};
Fixture fixture(int count, double gap) {
    Fixture f;
    f.sheet = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 200, 4, 1000).Shape();
    TopoDS_Compound punch;
    BRep_Builder b;
    b.MakeCompound(punch);
    const double length = (1000 - (count - 1) * gap) / count;
    for (int i = 0; i < count; ++i)
        b.Add(punch, BRepPrimAPI_MakeBox(gp_Pnt(98, 2, i * (length + gap)), 4, 50, length).Shape());
    f.punch = punch;
    BRepMesh_IncrementalMesh(f.sheet, .05);
    BRepMesh_IncrementalMesh(f.punch, .05);
    BRepAlgoAPI_Common c(f.sheet, f.punch);
    require(c.IsDone() && !c.HasErrors(), "Fixture common failed");
    f.overlap = c.Shape();
    return f;
}
PreviewFrame highlight(const Fixture &f) {
    PreviewFrame frame;
    frame.regions[0] = f.sheet;
    frame.tools[1] = f.punch;
    buildCollisionHighlights(frame, {{"sheet", "punch", f.overlap}}, 2, {});
    return frame;
}
std::size_t meshCount(const TopoDS_Shape &s) {
    std::size_t count = 0;
    for (TopExp_Explorer it(s, TopAbs_FACE); it.More(); it.Next()) {
        TopLoc_Location l;
        const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(it.Current()), l);
        if (!mesh.IsNull())
            count += mesh->NbTriangles();
    }
    return count;
}
float at(const PreviewFrame &frame, const std::string &body, const gp_Pnt &p) {
    float weight = 0;
    for (const auto &patch : frame.collisionHighlights) {
        if (patch.bodyId != body)
            continue;
        for (std::size_t i = 0; i < patch.vertices.size(); i += 3) {
            const auto &a = patch.vertices[i], &b = patch.vertices[i + 1],
                       &c = patch.vertices[i + 2];
            const auto ab = gp_Vec(a.point, b.point), ac = gp_Vec(a.point, c.point),
                       ap = gp_Vec(a.point, p);
            const auto n = ab.Crossed(ac);
            if (n.SquareMagnitude() < 1e-20 || std::abs(ap.Dot(n)) > 1e-7 * n.Magnitude())
                continue;
            const double aa = ab.Dot(ab), bb = ab.Dot(ac), cc = ac.Dot(ac),
                         denominator = aa * cc - bb * bb;
            if (std::abs(denominator) < 1e-20)
                continue;
            const double u = (ap.Dot(ab) * cc - ap.Dot(ac) * bb) / denominator,
                         v = (ap.Dot(ac) * aa - ap.Dot(ab) * bb) / denominator;
            if (u < -1e-7 || v < -1e-7 || u + v > 1 + 1e-7)
                continue;
            weight = std::max(weight, static_cast<float>(a.strength * (1 - u - v) + b.strength * u +
                                                         c.strength * v));
        }
    }
    return weight;
}
void bounded(const PreviewFrame &frame) {
    std::size_t total = 0;
    for (const auto &p : frame.collisionHighlights) {
        require(p.band.has_value(), "Long slender overlap did not get a strip");
        require(p.vertices.size() <= 2048 * 3 && p.vertices.size() % 3 == 0,
                "Strip exceeded its mesh budget");
        const auto lo = std::min((*p.band)[0].Z(), (*p.band)[1].Z()),
                   hi = std::max((*p.band)[0].Z(), (*p.band)[1].Z());
        for (const auto &v : p.vertices) {
            require(v.strength >= 0 && v.strength <= 1, "Invalid strip strength");
            require(v.point.Z() >= lo - 1e-6 && v.point.Z() <= hi + 1e-6,
                    "Strip painted beyond the confirmed axial interval");
            require(v.strength == 0 || (v.point.X() >= 89 && v.point.X() <= 111),
                    "Strip painted a distant part of the body");
        }
        total += p.vertices.size();
    }
    require(total <= 8192 * 3, "Total strip budget exceeded");
}
} // namespace
int main() {
    try {
        require(!collisionStrip(BRepPrimAPI_MakeBox(40, 40, 1000).Shape(), 8, {}),
                "A wide overlap was guessed to be a narrow strip");
        require(!collisionStrip(BRepPrimAPI_MakeSphere(20).Shape(), 8, {}),
                "A curved/degenerated boundary was guessed to be a straight strip");
        for (int n : {1, 4, 16, 32}) {
            const auto f = fixture(n, 0);
            const auto before = meshCount(f.sheet) + meshCount(f.punch);
            const auto frame = highlight(f);
            bounded(frame);
            require(frame.collisionHighlights.size() == static_cast<std::size_t>(2 * n),
                    "A confirmed component or participating surface was lost");
            for (double z : {5., 137., 499., 753., 995.}) {
                require(at(frame, "sheet", gp_Pnt(100, 4, z)) > .65,
                        "Long sheet collision has holes between endpoint spots");
                require(at(frame, "punch", gp_Pnt(98, 3, z)) > .7,
                        "Long tool collision was not marked continuously");
            }
            require(at(frame, "sheet", gp_Pnt(100, 4, 500)) >
                            at(frame, "sheet", gp_Pnt(104, 4, 500)) &&
                        at(frame, "sheet", gp_Pnt(120, 4, 500)) == 0,
                    "Strip lost its local transverse fade");
            require(before == meshCount(f.sheet) + meshCount(f.punch),
                    "Strip remeshed shared bodies");
        }
        const auto gaps = fixture(4, 3);
        const auto gapped = highlight(gaps);
        bounded(gapped);
        const double length = (1000 - 9.) / 4;
        for (int i = 0; i < 3; ++i) {
            const double middle = (i + 1) * length + i * 3 + 1.5;
            require(at(gapped, "sheet", gp_Pnt(100, 4, middle)) == 0,
                    "Strip bridged a physical section gap");
        }
        // The overlap may span a cutout, but the visible surface must retain the hole.
        auto holes = fixture(1, 0);
        TopoDS_Compound skin;
        BRep_Builder builder;
        builder.MakeCompound(skin);
        builder.Add(skin, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 200, 4, 490).Shape());
        builder.Add(skin, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 510), 200, 4, 490).Shape());
        builder.Add(skin, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 490), 96, 4, 20).Shape());
        builder.Add(skin, BRepPrimAPI_MakeBox(gp_Pnt(106, 0, 490), 94, 4, 20).Shape());
        BRepMesh_IncrementalMesh(skin, .05);
        PreviewFrame cutout;
        cutout.regions[0] = holes.sheet; // D029 checked copy; display retains its cutout.
        cutout.displayShape = skin;
        cutout.displayRegions[0] = skin;
        cutout.tools[1] = holes.punch;
        buildCollisionHighlights(cutout, {{"sheet", "punch", holes.overlap}}, 2, {});
        require(at(cutout, "sheet", gp_Pnt(100, 4, 480)) > .65 &&
                    at(cutout, "sheet", gp_Pnt(100, 4, 500)) == 0 &&
                    at(cutout, "sheet", gp_Pnt(100, 4, 520)) > .65,
                "Strip filled a cutout in the visible surface");
        const auto small = BRepPrimAPI_MakeBox(gp_Pnt(158, 2, 480), 4, 5, 4).Shape();
        BRepMesh_IncrementalMesh(small, .05);
        BRepAlgoAPI_Common smallCommon(holes.sheet, small);
        require(smallCommon.IsDone() && !smallCommon.HasErrors(), "Mixed fixture common failed");
        PreviewFrame mixed;
        mixed.regions[0] = holes.sheet;
        mixed.tools[1] = holes.punch;
        mixed.machine.push_back({"frame", "Frame", small});
        buildCollisionHighlights(
            mixed, {{"sheet", "punch", holes.overlap}, {"sheet", "frame", smallCommon.Shape()}}, 2,
            {});
        require(mixed.collisionHighlights.size() == 4 &&
                    at(mixed, "sheet", gp_Pnt(100, 4, 500)) > .65 &&
                    at(mixed, "sheet", gp_Pnt(160, 4, 482)) > .5,
                "A point or strip hid the simultaneous collision of another pair");
        // Translation follows the actual placement; direction is not a world Z assumption.
        gp_Trsf rotate;
        rotate.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .63);
        gp_Trsf translate;
        translate.SetTranslation(gp_Vec(300, -50, 900));
        const auto placement = translate * rotate;
        const auto original = fixture(1, 0);
        const Fixture moved{original.sheet.Moved(TopLoc_Location(placement)),
                            original.punch.Moved(TopLoc_Location(placement)),
                            original.overlap.Moved(TopLoc_Location(placement))};
        const auto shifted = highlight(moved);
        require(shifted.collisionHighlights.size() == 2 && shifted.collisionHighlights[0].band &&
                    shifted.collisionHighlights[1].band,
                "Rigid rotation lost its recognized strip");
        require(at(shifted, "sheet", gp_Pnt(100, 4, 500).Transformed(placement)) > .2,
                "Strip did not follow a rotated body");
        std::stop_source cancelled;
        cancelled.request_stop();
        PreviewFrame stopped;
        stopped.regions[0] = original.sheet;
        buildCollisionHighlights(stopped, {{"sheet", "punch", original.overlap}}, 2,
                                 cancelled.get_token());
        require(stopped.collisionHighlights.empty(), "Cancelled strip was published");
        std::vector<double> timings;
        for (int i = 0; i < 11; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto frame = highlight(gaps);
            timings.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        std::sort(timings.begin(), timings.end());
        std::cout << "Collision strips: continuity, real gaps, cutouts, immutable meshes, "
                     "rotation, budgets and cancellation passed; 4_sections_median_ms="
                  << timings[timings.size() / 2] << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
