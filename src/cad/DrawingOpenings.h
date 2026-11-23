#pragma once
#include "bending/application/BendingDrawing.h"
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

namespace bending::cad {
struct DrawingTubeGeometry;
struct DrawingTubeBore {
    double diameterMm;
    application::DrawingBoreShape shape;
};
struct DrawingOpening {
    TopoDS_Wire wire;
    gp_Pnt center;
    gp_Dir normal;
    gp_Vec major;
    gp_Vec minor;
    bool circular = false;
    std::vector<std::size_t> wallFaces;
    bool obround = false;
    int supportFace = 0;
    std::vector<int> windowWalls = {}; // Planar faces shared by the two rims of a through window.
    std::optional<gp_Dir> tubeOutward = {};
    std::optional<std::size_t> opposedBorePair = {};
    bool tubeWindow = false;
    bool straightBore = false;
    std::optional<DrawingTubeBore> tubeBore = {};
};
std::vector<DrawingOpening> drawingOpenings(const TopoDS_Shape &shape,
                                            const TopTools_IndexedMapOfShape &faces,
                                            std::stop_token stop, std::size_t maximumCandidates = 0,
                                            const DrawingTubeGeometry *tube = nullptr);
void addDrawingOpenings(const std::vector<DrawingOpening> &openings, const gp_Ax2 &projection,
                        const TopoDS_Shape &visible, application::DrawingView &view,
                        std::stop_token stop);
std::vector<std::size_t>
drawingContourRoundings(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                        const std::vector<application::RecognizedBend> &bends, double tolerance,
                        std::stop_token stop);
} // namespace bending::cad
