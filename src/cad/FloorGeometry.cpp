#include "FloorGeometry.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Pln.hxx>
#include <stdexcept>

namespace bending::cad {
FloorPlane makeFloorPlane(double y, double z) {
    constexpr double halfSize = 4000, spacing = 250;
    constexpr int count = 33;
    const gp_Pln plane(gp_Pnt(0, y, z), gp_Dir(0, 1, 0));
    auto face = BRepBuilderAPI_MakeFace(plane, -halfSize, halfSize, -halfSize, halfSize).Face();
    Handle(Poly_Triangulation) mesh =
        new Poly_Triangulation(count * count, 2 * (count - 1) * (count - 1), false, true);
    const auto index = [](int x, int row) { return 1 + x + count * row; };
    for (int j = 0; j < count; ++j)
        for (int i = 0; i < count; ++i) {
            mesh->SetNode(index(i, j),
                          gp_Pnt(-halfSize + i * spacing, y, z - halfSize + j * spacing));
            mesh->SetNormal(index(i, j), gp_Dir(0, 1, 0));
        }
    int triangle = 1;
    for (int j = 0; j + 1 < count; ++j)
        for (int i = 0; i + 1 < count; ++i) {
            mesh->SetTriangle(triangle++,
                              Poly_Triangle(index(i, j), index(i, j + 1), index(i + 1, j)));
            mesh->SetTriangle(triangle++,
                              Poly_Triangle(index(i + 1, j), index(i, j + 1), index(i + 1, j + 1)));
        }
    mesh->Deflection(0);
    BRep_Builder builder;
    builder.UpdateFace(face, mesh);
    TopoDS_Compound grid;
    builder.MakeCompound(grid);
    // Draw 500 mm grid lines; retain the finer surface mesh for contact overlays.
    for (int i = 0; i < count; i += 2) {
        const double offset = -halfSize + i * spacing;
        builder.Add(grid, BRepBuilderAPI_MakeEdge(gp_Pnt(offset, y + .05, z - halfSize),
                                                  gp_Pnt(offset, y + .05, z + halfSize))
                              .Edge());
        builder.Add(grid, BRepBuilderAPI_MakeEdge(gp_Pnt(-halfSize, y + .05, z + offset),
                                                  gp_Pnt(halfSize, y + .05, z + offset))
                              .Edge());
    }
    return {y, face, grid};
}
TopoDS_Shape floorOverlap(const TopoDS_Shape &sheet, const Bnd_Box &box, double y, double tolerance,
                          std::stop_token stop) {
    const auto checkStop = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("Floor check cancelled");
    };
    checkStop();
    if (!std::isfinite(y) || !std::isfinite(tolerance) || tolerance < 0 || box.IsVoid() ||
        box.IsOpen())
        throw std::runtime_error("Invalid floor check parameters");
    // Contact within the numerical allowance is not forbidden penetration.
    const double boundary = y - tolerance;
    const double volumeTolerance = std::max(1e-5, 8 * tolerance * tolerance * tolerance);
    if (!std::isfinite(boundary) || !std::isfinite(volumeTolerance))
        throw std::runtime_error("Invalid floor check range");
    if (box.CornerMin().Y() >= boundary)
        return {};
    TopoDS_Shape overlap;
    if (box.CornerMax().Y() < boundary) {
        overlap = sheet;
    } else {
        const auto plane =
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, boundary, 0), gp_Dir(0, 1, 0))).Face();
        const auto below = BRepPrimAPI_MakeHalfSpace(plane, gp_Pnt(0, boundary - 1, 0)).Solid();
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape first, second;
        first.Append(sheet);
        second.Append(below);
        common.SetArguments(first);
        common.SetTools(second);
        common.SetNonDestructive(true);
        common.SetRunParallel(false);
        common.SetFuzzyValue(tolerance);
        common.Build();
        checkStop();
        if (!common.IsDone() || common.HasErrors())
            throw std::runtime_error("Floor intersection failed; position is not verified");
        overlap = common.Shape();
    }
    if (!BRepCheck_Analyzer(overlap).IsValid())
        throw std::runtime_error("Invalid floor intersection; position is not verified");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(overlap, properties);
    if (!std::isfinite(properties.Mass()))
        throw std::runtime_error("Invalid floor intersection volume");
    return std::abs(properties.Mass()) > volumeTolerance ? overlap : TopoDS_Shape{};
}
} // namespace bending::cad
