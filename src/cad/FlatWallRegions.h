#pragma once
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace bending::application {
struct FaceInfo;
}

namespace bending::cad {
struct SheetWallRegion;

// Source face IDs are shared by measurements and cutout reconstruction.
[[nodiscard]] std::map<std::size_t, std::size_t>
mapSourceWallOwners(const TopoDS_Shape &original, const TopoDS_Shape &calculation,
                    std::span<const SheetWallRegion> walls,
                    std::span<const application::FaceInfo> metadata, bool differentTopology,
                    double tolerance, std::stop_token stop);

// One complete component per real pair of wall skins, with Cut history/area
// ownership. No rectangular approximation or unassigned residual material.
std::optional<std::vector<TopoDS_Shape>>
isolateFlatWalls(const TopoDS_Shape &, const std::vector<TopoDS_Shape> &,
                 const std::vector<std::array<TopoDS_Face, 2>> &, double, std::stop_token);
} // namespace bending::cad
