#pragma once
#include "bending/application/Document.h"
#include <TopoDS_Shape.hxx>
#include <functional>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <stop_token>

namespace bending::cad {
struct FlatDisplayRegion {
    TopoDS_Shape solid;
    std::function<gp_Pnt(gp_Pnt)> map;
};
struct FlatCutoutDisplay {
    TopoDS_Shape surface;
    std::vector<std::vector<application::PointMm>> contours;
};
// Display approximation of the original skin, independent of the filled calculation solid.
FlatCutoutDisplay flatCutoutDisplay(const TopoDS_Shape &source,
                                    const std::vector<FlatDisplayRegion> &regions, gp_Vec shift,
                                    double tolerance, std::stop_token stop);
} // namespace bending::cad
