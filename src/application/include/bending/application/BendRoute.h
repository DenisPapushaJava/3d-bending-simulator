#pragma once
#include "bending/application/Backgauge.h"
#include "bending/application/Document.h"
#include "bending/application/FlatPattern.h"
#include "bending/application/WorkpiecePlacement.h"
#include "bending/core/BendTarget.h"
#include "bending/core/ToolCatalog.h"
#include <cmath>
#include <functional>
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
    double workpieceOffsetMm = 0;
    bool operator==(const RouteOperation &) const = default;
};
struct RouteToolVariant {
    core::ToolAssignment assignment;
    // Null: common pair for the whole part; otherwise primary + additional zones only.
    std::optional<std::size_t> bendIndex;
    std::vector<std::size_t> additionalBendIndices = {};
};
[[nodiscard]] inline bool routeToolVariantApplies(const RouteToolVariant &variant,
                                                  std::size_t index) {
    if (!variant.bendIndex || *variant.bendIndex == index)
        return true;
    for (const auto member : variant.additionalBendIndices)
        if (member == index)
            return true;
    return false;
}
enum class BendRoutePhase { Preparing, ToolCandidate, Checking };
struct BendRouteProgress {
    BendRoutePhase phase = BendRoutePhase::Preparing;
    std::size_t variant = 0, variants = 0, checkedOperations = 0;
    std::optional<std::size_t> bendIndex;
};
// Explicit permission to change assemblies, preserving every captured profile,
// inventory, groove and installation parameter. Empty/absent means fixed tools.
struct RouteAssemblySearch {
    std::vector<std::size_t> bendIndices;
    double maximumGapMm = 0;
    bool allowDieWindows = false; // Only after an actual sheet-die collision.
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
    bool rankToolShapes = true; // Soft source-profile ranking; no feasibility pruning.
    bool reuseCalculations = true;
    bool allowJointBends = true; // Reference/diagnostic switch; no geometric rule is relaxed.
    std::optional<double> maximumEndSupportLossMm =
        std::nullopt; // Null with allowPartialEndSupport: measured outer end losses are warnings.
    // Explicit checked tool-selection command only. Empty preserves ordinary route behaviour.
    std::vector<RouteToolVariant> toolVariants;
    // Informational callback on the CAD worker. Not part of a geometric cache key.
    std::function<void(BendRouteProgress)> onProgress;
    std::optional<RouteAssemblySearch> assemblySearch = {};
    // Explicit automatic search only; fixed/manual requests retain their exact offset.
    bool allowAxialPlacement = false;
    bool reusePlacementPreparation = true; // Reference switch; no change to checked scope.
    bool reuseWorkpiecePreparation =
        true;                         // Exact intrinsic geometry/checks, no tool verdict reuse.
    bool rankToolAccess = true;       // Soft axial obstruction ranking; never a collision verdict.
    bool preferCommonAssembly = true; // Bounded original-assembly pass, then normal planning.
    // Explicit forming-only scope: operator extraction is not computed or certified.
    bool manualExtraction = false;
};
// Apply one input snapshot without changing IDs, targets, order or workpiece orientations.
[[nodiscard]] bool applyRouteToolVariant(BendRouteRequest &, std::size_t variant);
struct BendRouteStatistics {
    std::size_t builtStates = 0, reusedStates = 0;
    std::size_t builtTransitions = 0, reusedTransitions = 0;
    std::size_t reusedRejections =
        0; // Compact, exact-input reports; no diagnostic meshes retained.
    bool reusedFlatPattern = false;
    std::size_t reorderedStates = 0;
    bool usedProfilePriority = false;
    std::size_t testedToolVariants = 0;
    std::size_t gaugePrunedToolVariants = 0; // Proven axial-span rejection; no contact cycle.
    BackgaugeSearchStatistics backgauge;
    std::size_t shapeRankedToolVariants = 0;
    std::uint64_t toolShapeRankingUs = 0;
    std::size_t plannedAssemblies = 0, reusedAssemblyPlans = 0;
    std::size_t plannedDieWindows = 0, reusedDieWindowPlans = 0;
    std::size_t preparedOperations = 0, reusedOperationPreparations = 0;
    std::uint64_t operationPreparationUs = 0, placementEvaluationUs = 0;
    std::size_t preparedWorkpieces = 0, reusedWorkpieces = 0;
    std::size_t checkedWorkpieceSections = 0, reusedWorkpieceSections = 0;
    std::size_t toolAccessRankedStates = 0;
    std::size_t commonAssemblyPasses = 0, commonAssemblyChecks = 0;
    bool usedCommonAssembly = false;
};
struct BendRouteStep {
    std::string id;
    std::size_t bendIndex;
    bool fixedSecond; // Resolved workpiece Y half-turn (legacy field name), not punch orientation.
    std::shared_ptr<const SingleBendPreview> preview;
    // Additional original zones formed in this same press stroke. The primary
    // zone above keeps its stable ID; source recognition is never merged.
    // workpieceReversed is resolved relative to each member's own reference axis.
    std::vector<RouteOperation> additionalBends = {};
    // Only an explicitly authorized assembly search may change this snapshot.
    // Alias of an interned immutable input, not a new DXF copy for each state.
    std::shared_ptr<const core::ToolAssignment> assemblyAssignment = {};
};
struct BendRoute {
    DocumentPtr source;
    std::vector<BendRouteStep> steps;
    std::size_t evaluatedOperations = 0;
    bool withTools = false;
    bool partialEndSupport = false;
    BendRouteStatistics statistics = {};
    std::optional<std::size_t> selectedToolVariant;
    // The first route is geometric K=0.5; material BA remains a separate flat pattern.
    // Tool mode checks each cycle unless manualExtraction explicitly excludes punch return.
    // Operator extraction/repositioning and the actual machine are NOT certified.
    bool manualExtraction = false;
};
[[nodiscard]] bool validRouteAssemblySearch(const BendRouteRequest &);
[[nodiscard]] bool sameToolSetupExceptAssemblies(const core::ToolAssignment &,
                                                 const core::ToolAssignment &);
// Validate derived assemblies against the original permission/inventory, then
// atomically resolve expected inputs for normal preview validation and saving.
[[nodiscard]] bool applyRouteAssemblyResult(BendRouteRequest &, const BendRoute &);
// A common uninterrupted material strip on both flanges for the whole forming interval.
// Axial coordinates along this operation's bend line; end overhangs are NOT certified.
struct BendEndSupport {
    double bendLengthMm;
    double supportedFromMm;
    double supportedToMm;
};
// One real axial interval of an operation. Empty space between joint zones is
// not material and must not be collapsed into a single central support strip.
struct BendSupportInterval {
    double fromMm;
    BendEndSupport material;
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
    [[nodiscard]] bool coversBend(double toleranceMm = 0) const noexcept {
        return punchLengthMm + toleranceMm >= bendLengthMm &&
               dieLengthMm + toleranceMm >= bendLengthMm;
    }
    bool operator==(const BendRouteToolLengthIssue &) const = default;
};
struct BendRouteError {
    BendRouteErrorCode code;
    std::string detail;
    std::optional<std::size_t> bendIndex = {};
    std::size_t evaluatedOperations = 0;
    std::optional<BendPreviewErrorCode> operationError = {};
    std::optional<BendRouteToolLengthIssue> toolLengths = {};
    BendRouteStatistics statistics = {};
    std::optional<double> invariantGaugeSpanMm = {}; // Proven common-axis upper bound only.
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
    std::optional<BendRouteToolLengthIssue> toolLengths;
    // Actual reuse in this request, independent of whether the geometric basis was warm.
    bool reusedVisual = false;
};
using RouteDraftPtr = std::shared_ptr<const RouteDraft>;
using RouteDraftResult = std::variant<RouteDraftPtr, BendRouteError>;
} // namespace bending::application
