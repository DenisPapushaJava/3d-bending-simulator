#pragma once

#include "bending/application/Document.h"
#include <array>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>

namespace bending::application {
struct DrawingPoint {
    double x = 0;
    double y = 0;
};
struct DrawingPath {
    std::vector<DrawingPoint> points;
    bool hidden = false;
};
struct DrawingDimension {
    DrawingPoint start;
    DrawingPoint end;
    double valueMm = 0;
    std::string label; // Semantic ID, not an editable measured value.
};
struct DrawingAngle {
    DrawingPoint vertex;
    DrawingPoint first;
    DrawingPoint second;
    double radians = 0;
    double minimumRadiusMm = 0; // Keep the dimension arc outside the material.
};
struct DrawingView {
    std::vector<DrawingPath> paths;
    DrawingPoint minimum;
    DrawingPoint maximum;
    std::vector<DrawingDimension> dimensions;
    DrawingPoint sideAnchor;
    DrawingPoint sideTip;
    std::optional<std::array<double, 2>>
        omittedX; // Verified homogeneous span; dimensions stay full.
    std::vector<std::pair<DrawingPoint, double>> circleCenters;
    std::vector<std::pair<DrawingPoint, std::string>> labels;
    std::vector<DrawingAngle> angles;
    std::vector<std::vector<DrawingPoint>> sectionLoops;
    std::vector<std::pair<DrawingPoint, double>> detailCircles;
    std::vector<std::pair<DrawingPoint, std::size_t>> bendNotes;
};
struct DrawingBend {
    double turnRadians;
    double innerRadiusMm;
    double thicknessMm;
    double axialLengthMm;
    std::array<std::optional<double>, 2> outerFlangeMm;
    bool diagnostic = false;
    bool trimmedEnds = false;
    std::optional<double> interiorAngleRadians;
};
struct DrawingDetail {
    DrawingView view;
    std::size_t bendIndex;
    std::string designation;
};
struct DrawingRequest {
    std::optional<std::size_t> baseFace;
    bool reverseView = false;
    int quarterTurns = -1; // -1: choose the better in-plane orientation.
    bool allowBreaks = true;
    std::optional<double> manualThicknessMm;
    bool operator==(const DrawingRequest &) const = default;
};
struct BendingDrawing {
    std::array<DrawingView, 4> views; // Front, top, left: first-angle projection; isometric.
    std::vector<DrawingBend> bends;
    std::size_t baseFace = 0;
    bool automaticBase = true;
    bool reverseView = false;
    std::optional<double> thicknessMm;
    bool manualThickness = false;
    bool localThickness = false;
    std::optional<double> declaredThicknessMm;
    std::size_t unrecognizedCylindricalFaces = 0;
    std::string sourceName;
    std::vector<DrawingDetail> details;
};
using BendingDrawingPtr = std::shared_ptr<const BendingDrawing>;
struct DrawingError {
    std::string detail;
    bool cancelled = false;
};
using DrawingResult = std::variant<BendingDrawingPtr, DrawingError>;
class BendingDrawingBuilder {
  public:
    virtual ~BendingDrawingBuilder() = default;
    [[nodiscard]] virtual DrawingResult build(DocumentPtr document, DrawingRequest request,
                                              std::stop_token stop) const = 0;
};
// Largest standard scale fitting the three orthographic view boxes, including annotation space.
[[nodiscard]] std::optional<double> drawingScale(const BendingDrawing &drawing) noexcept;
[[nodiscard]] std::optional<double> standardDrawingScale(double maximum) noexcept;
} // namespace bending::application
