#pragma once

#include "bending/application/FlatPattern.h"

namespace bending::application {
struct DxfExportReference {
    std::filesystem::path path;
    bool includeBends = true;
    std::string bendLayer;
    int bendColor = 1;
};

// Numerical cut geometry for the DXF workflow only. No CAD shape or simulation state.
struct ProjectDxf {
    FlatPatternPtr flat;
    std::optional<DxfExportReference> exported;
};
} // namespace bending::application
