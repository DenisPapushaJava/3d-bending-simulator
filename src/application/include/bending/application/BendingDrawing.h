#pragma once

#include "bending/application/Document.h"
#include "bending/application/FlatPattern.h"
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
    // Actual contour points for projected dimensions; start/end define the measured axis.
    std::optional<std::array<DrawingPoint, 2>> witnesses = {};
    std::size_t pitchIntervals = 0; // Verified equally spaced coordinates; valueMm is full span.
    double pitchMm = 0;
};
struct DrawingAngle {
    DrawingPoint vertex;
    DrawingPoint first;
    DrawingPoint second;
    double radians = 0;
    double minimumRadiusMm = 0; // Keep the dimension arc outside the material.
};
struct DrawingSectionMarker {
    DrawingPoint first;
    DrawingPoint second;
    DrawingPoint viewDirection;
    std::string designation;
};
struct DrawingCenterAxes {
    DrawingPoint center;
    DrawingPoint first;
    DrawingPoint second;
};
struct DrawingDirectionNote {
    DrawingPoint anchor;
    std::size_t first;
    std::size_t second;
    double radians;
};
struct DrawingViewArrow {
    DrawingPoint anchor;
    DrawingPoint direction;
    std::string designation;
};
struct DrawingBendCallout {
    DrawingPoint anchor;
    std::vector<std::size_t> bends;
};
struct DrawingView {
    std::vector<DrawingPath> paths;
    DrawingPoint minimum;
    DrawingPoint maximum;
    std::vector<DrawingDimension> dimensions;
    // Centers of visible symmetric inner contours, from this view's minimum X/Y.
    // Equal coordinates share one dimension; these are not unfolded positions.
    std::vector<DrawingDimension> holeDimensions;
    std::vector<DrawingCenterAxes> openingAxes;
    std::vector<DrawingDimension> sectionDimensions;
    std::vector<DrawingDirectionNote> directionNotes;
    std::size_t visibleOpeningCount = 0;
    bool reverseProjection = false; // Auxiliary orthographic view from the opposite side.
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
    std::vector<DrawingSectionMarker> sectionMarkers;
    // Zones for which both adjacent dimensions and the true angle are represented.
    std::vector<std::size_t> dimensionedBends;
    std::optional<std::pair<DrawingPoint, DrawingPoint>> profileStart;
    std::vector<DrawingDimension> axialDimensions;
    std::vector<DrawingBendCallout> groupedBendNotes;
    std::vector<std::pair<DrawingPoint, double>> holeDiameters;
    std::vector<DrawingViewArrow> viewArrows;
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
    bool fullProfile = false;
};
struct DrawingProfileSegment {
    DrawingDimension dimension; // Outside dimension along the physical wall, not an X gauge.
    std::array<std::size_t, 2> wallFaces{};
    std::optional<std::size_t> previousBend;
    std::optional<std::size_t> nextBend;
};
struct DrawingProfile {
    DrawingView view; // Profile contours with thickness; closed holes stay on the source views.
    std::vector<DrawingProfileSegment> segments;
    std::vector<std::size_t> bends; // Traversal of this open profile, not a manufacturing order.
    std::vector<int> turnSigns;
    PointMm station{0, 0, 0};
    PointMm normal{0, 0, 1};
    DrawingPoint locator; // Same frame as the isometric source view.
    std::string designation;
};
struct DrawingMeasurementCoverage {
    std::size_t total = 0; // Recognized non-diagnostic bends only, not proof of full recognition.
    std::size_t dimensioned = 0;
    std::vector<std::size_t> missing;
};
struct DrawingSectionLocation {
    std::size_t sourceView = 0;
    DrawingSectionMarker marker;
    PointMm station{0, 0, 0};
    std::vector<std::size_t> bends;
};
struct DrawingSectionBasis {
    PointMm origin;
    PointMm right; // Unit directions, in the source model; normal is DrawingSection::normal.
    PointMm up;
    std::size_t sourceView;
};
struct DrawingSection {
    DrawingView view; // Actual intersection of the source solid, including internal voids.
    PointMm normal{0, 0, 1};
    std::string designation;
    std::vector<DrawingSectionLocation> locations;
    // A verified translated unit of this true section, not a claim of whole-body periodicity.
    std::size_t repeatCount = 0;
    bool rotated = false;
    std::optional<DrawingSectionBasis> basis = {};
};
struct DrawingAuxiliaryView {
    DrawingView view;
    std::string designation;
    std::size_t projection = 0;
};
enum class DrawingVariableProfile { None, Measured, NotMeasured };
struct DrawingRequest {
    std::optional<std::size_t> baseFace;
    bool reverseView = false;
    int quarterTurns = -1; // -1: choose the better in-plane orientation.
    bool allowBreaks = true;
    std::optional<double> manualThicknessMm;
    std::optional<core::UnfoldSettings> unfoldSettings = {};
    FlatPatternPtr preparedFlatPattern = {}; // Optional cache; not an input affecting equality.
    bool operator==(const DrawingRequest &other) const {
        return baseFace == other.baseFace && reverseView == other.reverseView &&
               quarterTurns == other.quarterTurns && allowBreaks == other.allowBreaks &&
               manualThicknessMm == other.manualThicknessMm &&
               unfoldSettings == other.unfoldSettings;
    }
};
struct DrawingFlatPattern {
    DrawingView view; // True flat contour, without HLR of the folded source.
    FlatPatternInfo info;
    // Groups of actual bend IDs sharing one phase of verified periodic line coordinates.
    std::vector<std::vector<std::size_t>> repeatedBendGroups;
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
    bool drawingOnlyRecognition = false;
    DrawingVariableProfile variableProfile = DrawingVariableProfile::None;
    BendRecognitionStatus bendRecognitionStatus = BendRecognitionStatus::NotAnalyzed;
    std::vector<DrawingProfile> profiles;
    std::optional<std::array<DrawingView, 3>> classicalViews;
    std::vector<DrawingSection> sections;
    std::vector<DrawingAuxiliaryView> auxiliaryViews;
    std::optional<DrawingFlatPattern> flatPattern = {};
    std::optional<FlatPatternError> flatPatternError = {};
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
[[nodiscard]] bool drawingProfilesCoverBends(const BendingDrawing &drawing) noexcept;
[[nodiscard]] DrawingMeasurementCoverage drawingMeasurementCoverage(const BendingDrawing &drawing,
                                                                    bool sectionsOnly = false);
} // namespace bending::application
