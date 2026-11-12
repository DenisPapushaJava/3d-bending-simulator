#pragma once
#include "bending/application/BendingDrawing.h"
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

namespace bending::cad {
struct DrawingOpening {
    TopoDS_Wire wire;
    gp_Pnt center;
    gp_Dir normal;
    gp_Vec major;
    gp_Vec minor;
    bool circular = false;
    std::vector<std::size_t> wallFaces;
    bool obround = false;
};
std::vector<DrawingOpening> drawingOpenings(const TopoDS_Shape &shape,
                                            const TopTools_IndexedMapOfShape &faces,
                                            std::stop_token stop,
                                            std::size_t maximumCandidates = 0);
void addDrawingOpenings(const std::vector<DrawingOpening> &openings, const gp_Ax2 &projection,
                        const TopoDS_Shape &visible, application::DrawingView &view,
                        std::stop_token stop);
std::vector<std::size_t>
drawingContourRoundings(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                        const std::vector<application::RecognizedBend> &bends, double tolerance,
                        std::stop_token stop);
} // namespace bending::cad
