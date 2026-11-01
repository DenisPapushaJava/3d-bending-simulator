#pragma once
#include <TopoDS_Shape.hxx>
#include <array>
#include <cstddef>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <vector>

namespace bending::cad {
enum class SheetSide : unsigned char { Neutral, A, B };
struct SheetEdgeMarker {
    std::array<gp_Pnt, 2> points;
    gp_Vec normal;
};
struct SheetReference {
    gp_Pnt point;
    gp_Vec normal;
    double thicknessMm = 0;
    std::vector<SheetEdgeMarker> edges;
};
struct SheetOrientationData {
    std::vector<SheetSide> faces; // Same one-based TopExp face map, index 0 unused.
    SheetReference reference;
    bool available = false;
};
struct SheetOrientationVisual {
    std::array<std::vector<TopoDS_Shape>, 2> sides; // Existing display faces; never new solids.
    std::vector<SheetEdgeMarker> edges;
};
} // namespace bending::cad
