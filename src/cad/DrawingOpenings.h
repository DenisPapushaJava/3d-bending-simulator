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
};
std::vector<DrawingOpening> drawingOpenings(const TopoDS_Shape &shape,
                                            const TopTools_IndexedMapOfShape &faces,
                                            std::stop_token stop);
void addDrawingOpenings(const std::vector<DrawingOpening> &openings, const gp_Ax2 &projection,
                        const TopoDS_Shape &visible, application::DrawingView &view,
                        std::stop_token stop);
} // namespace bending::cad
