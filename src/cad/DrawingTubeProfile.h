#pragma once
#include "bending/application/BendingDrawing.h"

#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>

namespace bending::cad {
struct DrawingTubeNotch {
    gp_Dir outward;
    std::array<gp_Pnt, 2> rearEdge;
    gp_Pnt endDatum;
    gp_Pnt sideDatum;
};
struct DrawingTubeCompoundEnd {
    std::size_t axialSide;
    std::vector<int> bevelFaces;
    std::optional<DrawingTubeNotch> notch;
};

struct DrawingTubeWall {
    int outerFace;
    int innerFace;
    gp_Dir outward;
};

struct DrawingTubeGeometry {
    application::DrawingTubeProfile profile;
    gp_Dir axis;
    std::size_t sideFace = 0; // A broad exterior planar wall, if present.
    std::array<TopoDS_Wire, 2> bores;
    std::array<int, 2> endFaces{};
    std::vector<int> longitudinalFaces;
    std::optional<std::array<DrawingTubeWall, 4>> planarWalls;
    std::optional<DrawingTubeCompoundEnd> compoundEnd;
    std::array<std::optional<double>, 2>
        cutsDegrees{}; // From the perpendicular, in increasing axial order.
};

std::optional<DrawingTubeGeometry> drawingTubeProfile(const TopoDS_Shape &shape,
                                                      const TopTools_IndexedMapOfShape &faces,
                                                      double tolerance, std::stop_token stop);

std::optional<std::array<double, 2>> drawingTubeBreak(const TopTools_IndexedMapOfShape &faces,
                                                      const TopTools_IndexedMapOfShape &edges,
                                                      const DrawingTubeGeometry &tube,
                                                      const gp_Dir &right, double low, double high,
                                                      std::stop_token stop);

struct DrawingTubeCutLocation {
    std::size_t projection;
    application::DrawingPoint anchor;
};
using DrawingTubeCutLocations = std::array<std::optional<DrawingTubeCutLocation>, 2>;

bool drawingTubeVisibleEdge(application::DrawingPoint, const application::DrawingView &,
                            std::stop_token);

DrawingTubeCutLocations addDrawingTubeCuts(const DrawingTubeGeometry &tube,
                                           const TopTools_IndexedMapOfShape &faces,
                                           const std::array<gp_Ax2, 3> &frames,
                                           const std::array<application::DrawingView *, 3> &views,
                                           std::stop_token stop);
} // namespace bending::cad
