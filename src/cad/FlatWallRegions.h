#pragma once
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <array>
#include <optional>
#include <stop_token>
#include <vector>

namespace bending::cad {
// One complete component per real pair of wall skins, with Cut history/area
// ownership. No rectangular approximation or unassigned residual material.
std::optional<std::vector<TopoDS_Shape>>
isolateFlatWalls(const TopoDS_Shape &, const std::vector<TopoDS_Shape> &,
                 const std::vector<std::array<TopoDS_Face, 2>> &, double, std::stop_token);
} // namespace bending::cad
