#pragma once
#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/core/ToolCatalog.h"
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
};
struct BendRouteStep {
    std::string id;
    std::size_t bendIndex;
    bool fixedSecond;
    std::shared_ptr<const SingleBendPreview> preview;
};
struct BendRoute {
    DocumentPtr source;
    std::vector<BendRouteStep> steps;
    std::size_t evaluatedOperations = 0;
    bool withTools = false;
    bool partialEndSupport = false;
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
};
using BendRouteResult = std::variant<BendRoutePtr, BendRouteError>;
} // namespace bending::application
