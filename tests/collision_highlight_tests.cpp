#include "CollisionHighlight.h"
#include "bending/cad/occt/MachineAssembly.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace bending::cad;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::size_t triangles(const TopoDS_Shape &shape) {
    std::size_t count = 0;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        TopLoc_Location location;
        const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(it.Current()), location);
        if (!mesh.IsNull())
            count += mesh->NbTriangles();
    }
    return count;
}
} // namespace
int main() {
    try {
        const auto sheet = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 200, 40, 4).Shape();
        const auto die = BRepPrimAPI_MakeBox(gp_Pnt(98, 35, 2), 4, 30, 50).Shape();
        BRepMesh_IncrementalMesh(sheet, .05);
        BRepMesh_IncrementalMesh(die, .05);
        const auto sheetTriangles = triangles(sheet), dieTriangles = triangles(die);
        BRepAlgoAPI_Common common(sheet, die);
        require(common.IsDone() && !common.HasErrors(), "Fixture intersection failed");
        PreviewFrame frame;
        frame.regions[0] = sheet;
        frame.tools[0] = die;
        buildCollisionHighlights(frame, {}, 2, {});
        require(frame.collisionHighlights.empty(), "Clear frame acquired a red patch");
        const std::vector<CollisionEvidence> evidence{{"sheet", "die", common.Shape()}};
        buildCollisionHighlights(frame, evidence, 2, {});
        require(frame.collisionHighlights.size() == 2,
                "Both colliding surfaces need local markers");
        std::size_t bytes = 0;
        for (const auto &patch : frame.collisionHighlights) {
            require(patch.vertices.size() <= 2048 * 3 && patch.vertices.size() % 3 == 0,
                    "Local mesh exceeded its per-body budget");
            require(patch.centres.size() <= 16 && patch.radiusMm == 32,
                    "Unexpected or unbounded marker region");
            float faintest = 1, brightest = 0;
            for (const auto &v : patch.vertices) {
                require(v.strength >= 0 && v.strength <= 1, "Invalid gradient weight");
                double distance = patch.radiusMm;
                for (const auto &p : patch.centres)
                    distance = std::min(distance, p.Distance(v.point));
                const double t = std::clamp(1 - distance / patch.radiusMm, 0., 1.);
                require(std::abs(v.strength - t * t * (3 - 2 * t)) < 1.e-6,
                        "Gradient is not derived from the localized collision");
                require(v.point.X() > 60 && v.point.X() < 140,
                        "A distant part of the body was painted red");
                faintest = std::min(faintest, v.strength);
                brightest = std::max(brightest, v.strength);
            }
            require(brightest > .8 && faintest < .1, "Marker lost its bright centre or soft edge");
            bytes += patch.vertices.capacity() * sizeof(CollisionHighlightVertex);
        }
        require(triangles(sheet) == sheetTriangles && triangles(die) == dieTriangles,
                "Marker remeshed a shared input body");
        PreviewFrame machine;
        machine.regions[0] = sheet;
        machine.machine.push_back({"frame-right", "Frame", die});
        buildCollisionHighlights(machine, {{"sheet", "frame-right", common.Shape()}}, 2, {});
        require(machine.collisionHighlights.size() == 2, "Machine collision was not localized");
        const auto other = BRepPrimAPI_MakeBox(gp_Pnt(158, 35, 2), 4, 30, 50).Shape();
        BRepMesh_IncrementalMesh(other, .05);
        TopoDS_Compound twoTools;
        BRep_Builder builder;
        builder.MakeCompound(twoTools);
        builder.Add(twoTools, die);
        builder.Add(twoTools, other);
        BRepAlgoAPI_Common disjoint(sheet, twoTools);
        require(disjoint.IsDone() && !disjoint.HasErrors(), "Two-contact fixture failed");
        PreviewFrame multiple;
        multiple.regions[0] = sheet;
        multiple.tools[0] = twoTools;
        buildCollisionHighlights(multiple, {{"sheet", "die", disjoint.Shape()}}, 2, {});
        require(multiple.collisionHighlights.size() == 4,
                "Only the first disconnected contact was marked");
        for (const auto &patch : multiple.collisionHighlights)
            require(std::any_of(patch.vertices.begin(), patch.vertices.end(),
                                [](const auto &v) { return v.strength > .8; }),
                    "A contact exhausted another contact's marker budget");
        PreviewFrame severalMachines;
        severalMachines.shape = sheet;
        severalMachines.regions[0] = sheet;
        severalMachines.tools[0] = BRepPrimAPI_MakeBox(gp_Pnt(-500, 0, 0), 1, 1, 1).Shape();
        severalMachines.tools[1] = BRepPrimAPI_MakeBox(gp_Pnt(-400, 0, 0), 1, 1, 1).Shape();
        severalMachines.machine = {{"left", "Left", die}, {"right", "Right", other}};
        const auto hits = MachineAssembly::collisions(severalMachines, 1.e-6, {});
        require(hits.size() == 2 && hits[0].pair.secondId == "left" &&
                    hits[1].pair.secondId == "right",
                "Only the first machine obstacle was checked");
        std::vector<CollisionEvidence> all;
        for (const auto &hit : hits)
            all.push_back({hit.pair.firstId, hit.pair.secondId, hit.overlap});
        buildCollisionHighlights(severalMachines, all, 2, {});
        require(severalMachines.collisionHighlights.size() == 4,
                "Simultaneous machine collisions lost their separate patches");
        gp_Trsf translation;
        translation.SetTranslation(gp_Vec(300, -50, 900));
        const TopLoc_Location location(translation);
        PreviewFrame moved;
        moved.regions[0] = sheet.Moved(location);
        moved.tools[0] = die.Moved(location);
        buildCollisionHighlights(moved, {{"sheet", "die", common.Shape().Moved(location)}}, 2, {});
        require(moved.collisionHighlights.size() == frame.collisionHighlights.size(),
                "Rigid placement lost a marker");
        for (std::size_t p = 0; p < frame.collisionHighlights.size(); ++p) {
            const auto &first = frame.collisionHighlights[p].vertices;
            const auto &second = moved.collisionHighlights[p].vertices;
            require(first.size() == second.size(), "Marker depends on world origin");
            for (std::size_t i = 0; i < first.size(); ++i)
                require(first[i].point.Transformed(translation).Distance(second[i].point) < 1.e-8 &&
                            std::abs(first[i].strength - second[i].strength) < 1.e-6,
                        "Marker did not follow the actual body placement");
        }
        std::vector<double> timings;
        for (int i = 0; i < 60; ++i) {
            PreviewFrame sample;
            sample.regions[0] = sheet;
            sample.tools[0] = die;
            const auto start = std::chrono::steady_clock::now();
            buildCollisionHighlights(sample, evidence, 2, {});
            timings.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        std::sort(timings.begin(), timings.end());
        std::cout
            << "Local collision surfaces, radial falloff, machine/rigid placement, bounds and "
               "immutable input mesh passed; patch median_ms="
            << timings[timings.size() / 2] << " p95_ms=" << timings[timings.size() * 95 / 100]
            << " reserved_bytes=" << bytes << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
