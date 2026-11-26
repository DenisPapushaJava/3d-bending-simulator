#pragma once

#include "bending/application/BendingDrawing.h"
#include <span>

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

struct DrawingTubeEndReliefs {
    std::array<std::vector<int>, 2> rimFaces; // Source faces, in increasing axial order.
    std::optional<double> mirrorStation;      // Axial mid-plane, confirmed by both differences.
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
    std::optional<DrawingTubeEndReliefs> endReliefs;
    std::array<std::optional<double>, 2>
        cutsDegrees{}; // From the perpendicular, in increasing axial order.
};

std::optional<DrawingTubeGeometry> drawingTubeProfile(const TopoDS_Shape &shape,
                                                      const TopTools_IndexedMapOfShape &faces,
                                                      double tolerance, std::stop_token stop);

std::optional<DrawingTubeGeometry>
drawingTubeProfileWithEndReliefs(const TopoDS_Shape &, const TopTools_IndexedMapOfShape &faces,
                                 const TopTools_IndexedMapOfShape &edges, double tolerance,
                                 std::stop_token);

// Reuse one axial scan when the same drawing needs more than one break.
class DrawingTubeBreaks {
  public:
    DrawingTubeBreaks(const TopTools_IndexedMapOfShape &faces,
                      const TopTools_IndexedMapOfShape &edges, const DrawingTubeGeometry &tube,
                      std::stop_token stop)
        : faces_(faces), edges_(edges), tube_(tube), stop_(stop) {}
    std::optional<std::array<double, 2>> find(const gp_Dir &right, double low, double high,
                                              std::span<const int> straightCutFaces = {});

  private:
    struct Feature {
        std::array<double, 2> axial;
        int face; // Zero for edges, which always participate.
    };
    std::vector<Feature> collect() const;

    const TopTools_IndexedMapOfShape &faces_;
    const TopTools_IndexedMapOfShape &edges_;
    const DrawingTubeGeometry &tube_;
    std::stop_token stop_;
    std::optional<std::vector<Feature>> features_;
};

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
