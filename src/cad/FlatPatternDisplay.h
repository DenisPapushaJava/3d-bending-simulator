#pragma once
#include "bending/application/Document.h"
#include "bending/cad/occt/SheetOrientation.h"
#include <Bnd_Box.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <functional>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <optional>
#include <span>
#include <stop_token>

namespace bending::cad {
// Bounded exact fallback for a source face whose point mappings disagree.
inline constexpr std::size_t sourceFaceRegionQueryLimit = 256;
std::optional<std::size_t> sourceFaceRegionOwner(const TopoDS_Face &, std::span<const TopoDS_Shape>,
                                                 std::span<const Bnd_Box>, double tolerance,
                                                 std::size_t &queries, std::stop_token);
struct FlatDisplayRegion {
    TopoDS_Shape solid;
    std::function<gp_Pnt(gp_Pnt)> map;
};
struct FlatCutoutDisplay {
    TopoDS_Shape surface;
    std::vector<std::vector<application::PointMm>> contours;
    std::vector<SheetSide> faceSides;
};
// Display approximation of the original skin, independent of the filled calculation solid.
FlatCutoutDisplay flatCutoutDisplay(const TopoDS_Shape &source,
                                    const std::vector<FlatDisplayRegion> &regions, gp_Vec shift,
                                    double tolerance, std::stop_token stop,
                                    const SheetOrientationData * = nullptr);
} // namespace bending::cad
