#pragma once

#include "bending/cad/CadDocument.h"

#include <TopoDS_Shape.hxx>

namespace bending::cad {
struct CadDocument::Impl {
    TopoDS_Shape shape;
    DocumentMetadata metadata;
    std::vector<application::FaceInfo> faces;
    std::string sourceStep;
};
} // namespace bending::cad
