#pragma once

#include "bending/application/Document.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <AIS_InteractiveObject.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <QStringList>

#include <optional>
#include <vector>

namespace bending::viewer {
struct DimensionAnnotations {
    std::vector<Handle(AIS_InteractiveObject)> objects;
    QStringList labels;
};
struct DimensionLayout {
    gp_Vec right; // Screen-space label separation expressed in world coordinates.
    gp_Vec up;
};
// Presentation only: measurements come from the immutable document/prepared frame.
[[nodiscard]] DimensionAnnotations
documentDimensions(const application::DocumentMetadata &, const TopTools_IndexedMapOfShape &,
                   const std::optional<application::RecognizedBend> &, double textHeight,
                   const DimensionLayout &);
[[nodiscard]] DimensionAnnotations previewDimensions(const cad::PreviewFrame &,
                                                     const simulation::SingleBend &,
                                                     double textHeight, const DimensionLayout &);
} // namespace bending::viewer
