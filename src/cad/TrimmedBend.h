#pragma once
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <functional>
#include <gp_Pnt.hxx>
#include <stdexcept>
#include <stop_token>

namespace bending::cad {
class TrimmedBendError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
// Rebuild ruled/planar compound ends from their actual cylindrical boundaries.
// The caller verifies the original reconstruction before publishing any motion.
// Optional nonRadialEnd identifies the new geometry class; it is not a proof.
TopoDS_Shape mapTrimmedBend(const TopoDS_Shape &, const std::function<gp_Pnt(gp_Pnt)> &,
                            double angleScale, double tolerance, TopoDS_Face *inner = nullptr,
                            std::stop_token stop = {}, bool *nonRadialEnd = nullptr);
} // namespace bending::cad
