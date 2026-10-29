#pragma once

#include "bending/cad/CadDocument.h"

#include <TopoDS_Shape.hxx>

namespace bending::cad {
struct CadDocument::Impl {
    TopoDS_Shape shape;
    TopoDS_Shape filledShape;
    application::BendAnalysis filledBends;
    TopoDS_Shape routeSkin; // Prepared triangulated skin of an internal route state only.
    bool routeState = false;
    DocumentMetadata metadata;
    std::vector<application::FaceInfo> faces;
    std::string sourceStep;
};
} // namespace bending::cad
