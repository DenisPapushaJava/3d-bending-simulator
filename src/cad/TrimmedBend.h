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
// Rebuild a radial ruled bend region from its actual cylindrical boundaries.
// The caller verifies the original reconstruction before publishing any motion.
TopoDS_Shape mapTrimmedBend(const TopoDS_Shape &, const std::function<gp_Pnt(gp_Pnt)> &,
                            double angleScale, double tolerance, TopoDS_Face *inner = nullptr,
                            std::stop_token stop = {});
} // namespace bending::cad
