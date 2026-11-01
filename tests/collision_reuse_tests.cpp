#include "CollisionReuse.h"
#include "bending/cad/occt/MachineAssembly.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <iostream>
#include <stdexcept>

using namespace bending::cad;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(shape, p);
    return std::abs(p.Mass());
}
int main() {
    try {
        const auto a = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
        const auto b = BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 10, 10, 10).Shape();
        const auto touch = BRepPrimAPI_MakeBox(gp_Pnt(10, 0, 0), 10, 10, 10).Shape();
        const auto thinHit = BRepPrimAPI_MakeBox(gp_Pnt(9.9999, 0, 0), 10, 10, 10).Shape();
        check(boundedDisjoint(a, b, .001), "Distant solids lost broad phase");
        check(!boundedDisjoint(a, touch, .001) && !boundedDisjoint(a, thinHit, .001),
              "Contact/penetration was certified from bounds");
        check(!boundedDisjoint({}, b, .001), "Missing bounds certified clearance");
        const auto loose = BRepPrimAPI_MakeBox(gp_Pnt(10.5, 0, 0), 10, 10, 10).Shape();
        BRep_Builder builder;
        for (TopExp_Explorer v(loose, TopAbs_VERTEX); v.More(); v.Next())
            builder.UpdateVertex(TopoDS::Vertex(v.Current()), 1);
        check(!boundedDisjoint(a, loose, .001), "Native shape tolerance was ignored");

        const auto distant = BRepPrimAPI_MakeBox(gp_Pnt(-1000, -1000, 0), 10, 10, 10).Shape();
        PreviewFrame base;
        base.shape = BRepPrimAPI_MakeBox(gp_Pnt(1000, 1000, 0), 10, 10, 10).Shape();
        base.tools = {distant, a};
        base.machine.push_back({"holder", "Holder", b, true, false, false});
        RigidClearanceCache memo;
        for (int i = 0; i < 8; ++i) {
            gp_Trsf move;
            if (i < 4)
                move.SetTranslation(gp_Vec(0, i * 17.25, i * 3.5));
            else
                move.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), .1 * i);
            const TopLoc_Location shared(move);
            auto frame = base;
            frame.tools[1] = a.Moved(shared);
            frame.machine[0].shape = b.Moved(TopLoc_Location(move));
            const auto reference = MachineAssembly::collisions(frame, .001, {});
            const auto reused = MachineAssembly::collisions(frame, .001, {}, &memo);
            check(reference.empty() && reused.empty(), "Common rigid movement changed clearance");
        }
        check(memo.hits() >= 6, "Rigid pair checks were not reused");
        check(!memo.contains(a, b, .002), "Changed tolerance reused a proof");
        check(!memo.contains(BRepBuilderAPI_Copy(a, true, true).Shape(), b, .001),
              "Different geometry reused a proof");
        check(!memo.contains(a.Reversed(), b, .001), "Changed orientation reused a proof");
        gp_Trsf relative;
        relative.SetTranslation(gp_Vec(-15, 0, 0));
        auto conflict = base;
        conflict.machine[0].shape = b.Moved(TopLoc_Location(relative));
        check(!memo.contains(conflict.tools[1], conflict.machine[0].shape, .001),
              "Relative movement reused a proof");
        const auto reference = MachineAssembly::collisions(conflict, .001, {});
        const auto reused = MachineAssembly::collisions(conflict, .001, {}, &memo);
        check(reference.size() == 1 && reused.size() == 1 &&
                  reference[0].pair.firstId == reused[0].pair.firstId &&
                  reference[0].pair.secondId == reused[0].pair.secondId &&
                  std::abs(volume(reference[0].overlap) - 500) < 1.e-6 &&
                  std::abs(volume(reused[0].overlap) - 500) < 1.e-6,
              "Collision pair/contact geometry changed");
        std::cout
            << "Conservative bounds, rigid reuse, relative changes and exact overlaps passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
