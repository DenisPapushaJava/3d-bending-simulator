#pragma once

#include "bending/application/BendingDrawing.h"
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

namespace bending::cad {
// Fold the reference plane onto the cut about their common trace, with no
// additional paper rotation. N points to the observer; the view looks along -N.
[[nodiscard]] gp_Ax2 drawingSectionFrame(gp_Pnt origin, gp_Dir normal, const gp_Ax2 &reference);
struct DrawingBendCorner {
    gp_Pnt point;
    std::array<gp_Dir, 2> directions;
    double interior;
};
[[nodiscard]] std::optional<DrawingBendCorner>
drawingBendCorner(const application::RecognizedBend &bend, const TopTools_IndexedMapOfShape &faces,
                  gp_Pnt station);
[[nodiscard]] std::vector<application::DrawingProfile>
drawingProfiles(const std::vector<application::RecognizedBend> &bends,
                const TopTools_IndexedMapOfShape &faces, const gp_Ax2 &reference, double tolerance,
                std::stop_token stop);
[[nodiscard]] std::optional<application::DrawingProfile>
drawingProfileAt(const std::vector<std::size_t> &members,
                 const std::vector<application::RecognizedBend> &bends,
                 const TopTools_IndexedMapOfShape &faces, const gp_Ax2 &reference, double tolerance,
                 double fraction, std::stop_token stop);
} // namespace bending::cad
