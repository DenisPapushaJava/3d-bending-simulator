#pragma once

#include "bending/cad/CadDocument.h"

#include <TopoDS_Shape.hxx>

namespace bending::cad {
struct CadDocument::Impl {
    TopoDS_Shape shape;
    DocumentMetadata metadata;
};
} // namespace bending::cad
