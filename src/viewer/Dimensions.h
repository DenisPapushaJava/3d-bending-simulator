#pragma once

#include "bending/application/Document.h"
#include "bending/application/FlatPattern.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <AIS_InteractiveObject.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <functional>

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
    std::function<QPointF(const gp_Pnt &)> project;
    QRectF viewport;
    std::vector<QRectF> excluded;
};
// Screen picking of prepared flat centre lines; no CAD selection/measurement.
[[nodiscard]] std::optional<std::size_t> flatBendAt(const application::FlatPatternInfo &,
                                                    const DimensionLayout &, QPointF,
                                                    double tolerancePixels);
// Presentation only: measurements come from the immutable document/prepared frame.
[[nodiscard]] DimensionAnnotations
documentDimensions(const application::DocumentMetadata &, const TopTools_IndexedMapOfShape &,
                   const std::optional<application::RecognizedBend> &, double textHeight,
                   const DimensionLayout &);
[[nodiscard]] DimensionAnnotations previewDimensions(const cad::PreviewFrame &,
                                                     const simulation::SingleBend &,
                                                     double textHeight, const DimensionLayout &,
                                                     bool compact = false);
[[nodiscard]] DimensionAnnotations flatPatternDimensions(const application::FlatPatternInfo &,
                                                         double textHeight, const DimensionLayout &,
                                                         std::optional<std::size_t> selected,
                                                         double facing);
} // namespace bending::viewer
