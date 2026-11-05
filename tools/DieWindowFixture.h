#pragma once
// Original MIT example: two coaxial bends and a reverse central tab on their shared web.
#include "BendFixtures.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <stdexcept>
namespace bending::fixtures {
inline TopoDS_Shape dieWindowPart() {
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0)));
    auto tab =
        BRepBuilderAPI_Transform(bentStrip(4, 2, std::numbers::pi / 2, 10), mirror, true).Shape();
    gp_Trsf move;
    move.SetTranslation(gp_Vec(0, 0, 50));
    tab = tab.Moved(TopLoc_Location(move));
    BRepAlgoAPI_Fuse join(jointBend(), tab);
    join.SimplifyResult(true, true);
    if (!join.IsDone() || !BRepCheck_Analyzer(join.Shape()).IsValid())
        throw std::runtime_error("Window fixture is invalid");
    return join.Shape();
}
} // namespace bending::fixtures
