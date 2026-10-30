#pragma once
#include "bending/cad/occt/ToolGeometryCacheAccess.h"
#include <sstream>

namespace bending::cad {
std::ostringstream geometryKey(std::string_view kind);
TopoDS_Shape cachedToolShape(const std::string &, const std::function<TopoDS_Shape()> &,
                             double deflection = .05);
} // namespace bending::cad
