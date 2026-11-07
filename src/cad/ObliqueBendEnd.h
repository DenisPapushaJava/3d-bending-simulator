#pragma once
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <array>
#include <functional>
#include <gp_Pnt.hxx>
#include <stop_token>
#include <vector>

namespace bending::cad {
// Only derived edges are updated; imported faces/curves remain immutable.
TopoDS_Face mapObliqueBendEnd(const TopoDS_Face &, const TopTools_IndexedMapOfShape &,
                              std::vector<TopoDS_Edge> &, const std::array<TopoDS_Face, 2> &,
                              const std::function<gp_Pnt(gp_Pnt)> &, double, std::stop_token);
} // namespace bending::cad
