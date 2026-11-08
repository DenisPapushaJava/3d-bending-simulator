#pragma once
#include "bending/application/Backgauge.h"

#include "bending/application/BendRoute.h"
#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/core/ToolCatalog.h"
#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/PressMotion.h"
#include "bending/simulation/SingleBend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <variant>

namespace bending::application {
enum class BendPreviewMode { Kinematic, ToolSetup, ToolMotion };
// Published only after a continuous swept-volume check of the complete vertical
// punch return. Clearance is the actual final minimum distance to the sheet.
struct PunchRetractionCheck {
    double liftMm;
    double finalClearanceMm;
};
enum class ToolCollisionPair { SheetDie, SheetPunch, DiePunch };
struct MachineCollision {
    std::string firstId, firstName, secondId, secondName;
};
class SingleBendPreview {
  public:
    [[nodiscard]] virtual std::size_t activeBendCount() const noexcept {
        return 1;
    }
    virtual ~SingleBendPreview() = default;
    [[nodiscard]] virtual const BackgaugeReport *backgauge() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual bool workpieceReversed() const noexcept {
        return false;
    }
    [[nodiscard]] virtual double workpieceOffsetMm() const noexcept {
        return 0;
    }
    // True only when every published position includes the checked floor scope.
    [[nodiscard]] virtual bool floorChecked() const noexcept {
        return false;
    }
    [[nodiscard]] virtual bool seatedPunchMount() const noexcept {
        return false;
    }
    [[nodiscard]] virtual core::MachineModel machineModel() const noexcept {
        return core::MachineModel::None;
    }
    [[nodiscard]] virtual std::optional<MachineCollision>
    machineCollisionAtFrame(std::size_t) const {
        return {};
    }
    [[nodiscard]] virtual bool unverifiedDraft() const noexcept {
        return false;
    }
    // One operation: every other bend remains in its imported formed state.
    [[nodiscard]] virtual std::size_t otherFormedBends() const noexcept {
        return 0;
    }
    // Maximum actual joint gap in each assembly (die, punch), not uniform spacing.
    [[nodiscard]] virtual std::array<double, 2> toolSegmentGaps() const noexcept {
        return {0, 0};
    }
    // Axial length of active bend material without tool, independently for die/punch.
    // Geometric accounting only; no strength/deformation claim for the remaining material.
    [[nodiscard]] virtual std::array<double, 2> uncoveredToolLengthMm() const noexcept {
        return {0, 0};
    }
    [[nodiscard]] virtual std::span<const BendSupportInterval>
    endSupportIntervals() const noexcept {
        return {};
    }
    // Present only when shorter continuous tools cover the complete measured core.
    [[nodiscard]] virtual std::optional<std::array<double, 2>> axialToolCoreMm() const noexcept {
        return {};
    }
    [[nodiscard]] bool hasPartialEndSupport() const noexcept {
        for (const auto &interval : endSupportIntervals())
            if (interval.material.supportedFromMm > 0 ||
                interval.material.supportedToMm < interval.material.bendLengthMm)
                return true;
        return false;
    }
    // Null: measured external end losses are warnings. Zero: full support only.
    // This snapshot belongs to the checked cycle, not the current editor setting.
    [[nodiscard]] virtual std::optional<double> endSupportLimitMm() const noexcept {
        return 0;
    }
    [[nodiscard]] virtual bool usesDrawingTools() const noexcept {
        return false;
    }
    [[nodiscard]] virtual bool usesDrawingPunch() const noexcept {
        return usesDrawingTools();
    }
    [[nodiscard]] virtual bool usesDrawingDie() const noexcept {
        return usesDrawingTools();
    }
    [[nodiscard]] virtual std::optional<ToolCollisionPair> collision() const noexcept {
        return {};
    }
    [[nodiscard]] virtual std::optional<ToolCollisionPair>
    collisionAtFrame(std::size_t) const noexcept {
        return collision();
    }
    [[nodiscard]] virtual bool ignoresClosedCutouts() const noexcept {
        return false;
    }
    [[nodiscard]] virtual const simulation::PressMotion *pressMotion() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const simulation::PressState *pressState(std::size_t) const noexcept {
        return nullptr;
    }
    // Forming was checked, but no punch-return or operator-extraction path was verified.
    [[nodiscard]] virtual bool manualExtraction() const noexcept {
        return false;
    }
    [[nodiscard]] virtual const PunchRetractionCheck *punchRetraction() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const simulation::SingleBend &model() const noexcept = 0;
    [[nodiscard]] virtual std::size_t frameCount() const noexcept = 0;
    [[nodiscard]] virtual BoundsMm bounds() const noexcept = 0;
    // Non-null in both tool modes. A setup without pressMotion is a static one-frame view.
    [[nodiscard]] virtual const simulation::FlatToolSetup *toolSetup() const noexcept {
        return nullptr;
    }
};
// Soft comparison of already measured support, never a manufacturing threshold.
// Gaps between joint zones do not dilute either the worst or total loss ratio.
[[nodiscard]] inline std::array<double, 2>
endSupportLossRank(const SingleBendPreview &preview) noexcept {
    double worst = 0, missing = 0, length = 0;
    for (const auto &interval : preview.endSupportIntervals()) {
        const auto &s = interval.material;
        if (!withinEndSupportLimit(s, s.bendLengthMm))
            return {1, 1};
        const double loss = s.supportedFromMm + (s.bendLengthMm - s.supportedToMm);
        worst = std::max(worst, loss / s.bendLengthMm);
        missing += loss;
        length += s.bendLengthMm;
    }
    return {worst, length > 0 ? missing / length : 0};
}
// Validate the transient CAD report against the operation envelope, independently
// of whether any of its real intervals has partial support. No gap is filled.
[[nodiscard]] inline bool validEndSupportIntervals(const SingleBendPreview &preview,
                                                   std::optional<double> limit,
                                                   double tolerance) noexcept {
    const auto intervals = preview.endSupportIntervals();
    if (intervals.empty())
        return !preview.toolSetup();
    const double width = preview.model().widthMm;
    if (!preview.toolSetup() || preview.endSupportLimitMm() != limit ||
        intervals.size() != preview.activeBendCount() || !std::isfinite(width) || width <= 0 ||
        !validEndSupportLimitMm(tolerance) || (limit && !validEndSupportLimitMm(*limit)))
        return false;
    double end = 0;
    for (const auto &interval : intervals) {
        const auto &s = interval.material;
        if (!std::isfinite(interval.fromMm) || interval.fromMm < 0 ||
            interval.fromMm < end - tolerance ||
            !withinEndSupportLimit(s, limit.value_or(width), tolerance))
            return false;
        end = interval.fromMm + s.bendLengthMm;
        if (!std::isfinite(end) || end > width + tolerance)
            return false;
    }
    return intervals.front().fromMm <= tolerance && std::abs(end - width) <= tolerance;
}
using BendPreviewPtr = std::shared_ptr<const SingleBendPreview>;
enum class BendPreviewErrorCode {
    UnsupportedGeometry,
    BendZoneInteriorContours,
    UnsupportedParameters,
    UnsupportedToolGeometry,
    NarrowDieOpening,
    TargetMismatch,
    ToolTooShort,
    MissingLeftSupport,
    MissingRightSupport,
    ToolAngleConflict,
    NoseTooLarge,
    SupportNotProven,
    ToolIntersection,
    MachineIntersection,
    ContactLost,
    SheetIntersection,
    RetractionNotProven,
    Cancelled,
    Failed,
    EndSupportLimitExceeded,
    BackgaugeUnavailable
};
struct BendPreviewError {
    BendPreviewErrorCode code;
    std::string detail;
    std::optional<std::size_t> frameIndex = std::nullopt;
    BendPreviewPtr diagnostic = {}; // Safe prefix + failed state, never a successful ToolMotion.
    std::optional<BendEndSupport> endSupport = std::nullopt;
    // Tool coverage of the support strip may change with workpiece Z.
    bool axialSupportGap = false;
    std::optional<ToolCollisionPair> toolCollision = std::nullopt;
    // Proposal from the actual Common at this exact pose; never a clearance certificate.
    std::optional<core::ToolWindow> dieWindow = std::nullopt;
};
using BendPreviewResult = std::variant<BendPreviewPtr, BendPreviewError>;
class SingleBendPreviewBuilder {
  public:
    // Drop retained route work on close/new import. Must not wait for an active CAD call.
    virtual void discardRouteCache() const {}
    [[nodiscard]] virtual RouteDraftResult buildRouteDraft(DocumentPtr, RouteDraftRequest,
                                                           std::stop_token) const {
        return BendRouteError{BendRouteErrorCode::Unsupported, "Draft preview unavailable"};
    }
    [[nodiscard]] virtual BendRouteResult buildRoute(DocumentPtr, BendRouteRequest,
                                                     std::stop_token) const {
        return BendRouteError{BendRouteErrorCode::Unsupported, "Route builder unavailable"};
    }
    [[nodiscard]] virtual BendPreviewResult
    buildAssignedToolSetup(DocumentPtr document, std::size_t index, core::BendTarget target,
                           core::ToolAssignment assignment, std::stop_token stop,
                           std::optional<double> maximumEndSupportLossMm = 0,
                           bool workpieceReversed = false, double workpieceOffsetMm = 0) const {
        if (assignment.useDrawingGeometry || assignment.punchAssembly || assignment.dieAssembly)
            return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                    "DXF setup unavailable"};
        const auto tools = core::toolParameters(assignment);
        return tools ? buildToolSetup(document, index, target, *tools, stop,
                                      maximumEndSupportLossMm, workpieceReversed, workpieceOffsetMm)
                     : BendPreviewResult(BendPreviewError{
                           BendPreviewErrorCode::UnsupportedParameters, "Invalid assignment"});
    }

    virtual ~SingleBendPreviewBuilder() = default;
    [[nodiscard]] virtual BendPreviewResult
    buildAssignedToolMotion(DocumentPtr document, std::size_t index, core::BendTarget target,
                            core::ToolAssignment assignment, std::stop_token stop,
                            std::optional<double> maximumEndSupportLossMm = 0,
                            bool workpieceReversed = false, double workpieceOffsetMm = 0) const {
        if (assignment.useDrawingGeometry || assignment.punchAssembly || assignment.dieAssembly)
            return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                    "DXF tool contact unavailable"};
        const auto tools = core::toolParameters(assignment);
        return tools
                   ? buildToolMotion(document, index, target, *tools, stop, maximumEndSupportLossMm,
                                     workpieceReversed, workpieceOffsetMm)
                   : BendPreviewResult(BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                                        "Invalid assignment"});
    }
    [[nodiscard]] virtual BendPreviewResult
    buildToolMotion(DocumentPtr, std::size_t, core::BendTarget, core::PressTools, std::stop_token,
                    std::optional<double> = 0, bool = false, double = 0) const {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                "Tool motion unavailable"};
    }
    [[nodiscard]] virtual BendPreviewResult
    buildToolSetup(DocumentPtr, std::size_t, core::BendTarget, core::PressTools, std::stop_token,
                   std::optional<double> = 0, bool = false, double = 0) const {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                "Tool setup unavailable"};
    }
    // Same source, confirmed zone and target. No writes to the source CAD topology.
    [[nodiscard]] virtual BendPreviewResult build(DocumentPtr document, std::size_t bendIndex,
                                                  core::BendTarget target, bool fixedSecond,
                                                  std::stop_token stop) const = 0;
};
} // namespace bending::application
