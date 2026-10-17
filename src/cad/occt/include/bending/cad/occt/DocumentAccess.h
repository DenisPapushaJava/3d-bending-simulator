#pragma once

#include "bending/cad/CadDocument.h"
#include <TopoDS_Shape.hxx>

namespace bending::cad {
// Narrow native bridge for the presenter. Borrowed for the document's lifetime.
// Display only after worker preparation finishes; do not modify shared topology.
class OcctDocumentAccess final {
  public:
    [[nodiscard]] static const TopoDS_Shape &shape(const CadDocument &document);
};
} // namespace bending::cad
