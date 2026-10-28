#pragma once
#include "bending/application/FlatPattern.h"
#include <TopoDS_Shape.hxx>

namespace bending::cad {
class OcctFlatPatternAccess final {
  public:
    [[nodiscard]] static bool belongsTo(const application::FlatPattern &,
                                        const application::Document &);
    // Borrowed, independently built and meshed in the CAD worker.
    [[nodiscard]] static const TopoDS_Shape &shape(const application::FlatPattern &);
    [[nodiscard]] static const std::vector<TopoDS_Shape> &
    bendLines(const application::FlatPattern &);
};
} // namespace bending::cad
