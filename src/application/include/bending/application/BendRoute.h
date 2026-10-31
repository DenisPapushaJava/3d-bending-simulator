#pragma once
#include "bending/application/Document.h"
#include "bending/application/FlatPattern.h"
#include "bending/core/BendTarget.h"
#include "bending/core/ToolCatalog.h"
#include <cmath>
#include <optional>
#include <stop_token>
#include <variant>

namespace bending::application {
class SingleBendPreview;
enum class BendPreviewErrorCode;
struct RouteOperation {
    std::string id;
    std::size_t bendIndex;
    core::BendTarget target;
    std::optional<core::PressTools> tools;
    std::optional<core::ToolAssignment> assignment;
    bool workpieceReversed = false;
    bool operator==(const RouteOperation &) const = default;
};
struct BendRouteRequest {
    // Complete list in the requested forward order; Automatic may reorder it.
    std::vector<RouteOperation> operations;
    bool automatic = false;
    bool withTools = false;
    bool allowPartialEndSupport = false; // Explicit geometric-preview scope, never a force check.
    std::optional<double> manualThicknessMm;
    std::size_t maximumChecks = 96;
    unsigned maximumTimeMs = 60000;
    // Diagnostic/reference switches; the UI uses both optimizations.
    bool allowWorkpieceRotation = true; // False: check exactly the requested installations.
    bool rankCandidates = true;
    bool reuseCalculations = true;
    std::optional<double> maximumEndSupportLossMm =
        std::nullopt; // Null: legacy explicitly unbounded preview.
};
struct BendRouteStatistics {
    std::size_t builtStates = 0, reusedStates = 0;
    std::size_t builtTransitions = 0, reusedTransitions = 0;
    std::size_t reusedRejections =
        0; // Compact, exact-input reports; no diagnostic meshes retained.
    bool reusedFlatPattern = false;
    std::size_t reorderedStates = 0;
    bool usedProfilePriority = false;
};
struct BendRouteStep {
    std::string id;
    std::size_t bendIndex;
    bool fixedSecond; // Resolved workpiece Y half-turn (legacy field name), not punch orientation.
    std::shared_ptr<const SingleBendPreview> preview;
};
struct BendRoute {
    DocumentPtr source;
    std::vector<BendRouteStep> steps;
    std::size_t evaluatedOperations = 0;
    bool withTools = false;
    bool partialEndSupport = false;
    BendRouteStatistics statistics = {};
    // The first route is geometric K=0.5; material BA remains a separate flat pattern.
    // Tool mode checks each cycle; extraction/repositioning/machine are NOT certified.
};
// A common uninterrupted material strip on both flanges for the whole forming interval.
// Axial coordinates along this operation's bend line; end overhangs are NOT certified.
struct BendEndSupport {
    double bendLengthMm;
    double supportedFromMm;
    double supportedToMm;
};
[[nodiscard]] inline bool validEndSupportLimitMm(double limit) noexcept {
    return std::isfinite(limit) && limit >= 0;
}
[[nodiscard]] inline bool withinEndSupportLimit(const BendEndSupport &s, double limit,
                                                double toleranceMm = 0) noexcept {
    return validEndSupportLimitMm(limit) && validEndSupportLimitMm(toleranceMm) &&
           std::isfinite(s.bendLengthMm) && s.bendLengthMm > 0 &&
           std::isfinite(s.supportedFromMm) && std::isfinite(s.supportedToMm) &&
           s.supportedFromMm >= 0 && s.supportedFromMm < s.bendLengthMm / 2 &&
           s.supportedToMm > s.bendLengthMm / 2 && s.supportedToMm <= s.bendLengthMm &&
           s.supportedFromMm <= limit + toleranceMm &&
           s.bendLengthMm - s.supportedToMm <= limit + toleranceMm;
}
using BendRoutePtr = std::shared_ptr<const BendRoute>;
enum class BendRouteErrorCode {
    Unsupported,
    InvalidInput,
    NoRoute,
    SearchLimit,
    Cancelled,
    Failed
};
struct BendRouteToolLengthIssue {
    double bendLengthMm;
    double punchLengthMm;
    double dieLengthMm;
};
struct BendRouteError {
    BendRouteErrorCode code;
    std::string detail;
    std::optional<std::size_t> bendIndex = {};
    std::size_t evaluatedOperations = 0;
    std::optional<BendPreviewErrorCode> operationError = {};
    std::optional<BendRouteToolLengthIssue> toolLengths = {};
    BendRouteStatistics statistics = {};
};
using BendRouteResult = std::variant<BendRoutePtr, BendRouteError>;
// Display-only prefix. Its visual has unverifiedDraft()==true and cannot certify a route.
struct RouteDraftRequest {
    std::vector<RouteOperation> operations;
    std::size_t step = 0;
    bool withTools = false;
    std::optional<double> manualThicknessMm;
    bool operator==(const RouteDraftRequest &) const = default;
};
struct RouteDraft {
    DocumentPtr source;
    RouteDraftRequest request;
    std::shared_ptr<const SingleBendPreview> visual;
    bool reusedBasis = false;
    std::size_t checkedToolPositions = 0;
    std::string toolNotice;
};
using RouteDraftPtr = std::shared_ptr<const RouteDraft>;
using RouteDraftResult = std::variant<RouteDraftPtr, BendRouteError>;
} // namespace bending::application
