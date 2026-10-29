#pragma once

#include "bending/application/BendRoute.h"
#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/core/ToolCatalog.h"
#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/PressMotion.h"
#include "bending/simulation/SingleBend.h"

#include <cstddef>
#include <optional>
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
class SingleBendPreview {
  public:
    virtual ~SingleBendPreview() = default;
    // One operation: every other bend remains in its imported formed state.
    [[nodiscard]] virtual std::size_t otherFormedBends() const noexcept {
        return 0;
    }
    [[nodiscard]] virtual std::array<double, 2> toolSegmentGaps() const noexcept {
        return {0, 0};
    }
    [[nodiscard]] virtual const BendEndSupport *partialEndSupport() const noexcept {
        return nullptr;
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
    [[nodiscard]] virtual bool ignoresClosedCutouts() const noexcept {
        return false;
    }
    [[nodiscard]] virtual const simulation::PressMotion *pressMotion() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const simulation::PressState *pressState(std::size_t) const noexcept {
        return nullptr;
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
    ContactLost,
    SheetIntersection,
    RetractionNotProven,
    Cancelled,
    Failed
};
struct BendPreviewError {
    BendPreviewErrorCode code;
    std::string detail;
    std::optional<std::size_t> frameIndex = std::nullopt;
    BendPreviewPtr diagnostic = {}; // Failed state only, never a successful ToolMotion.
};
using BendPreviewResult = std::variant<BendPreviewPtr, BendPreviewError>;
class SingleBendPreviewBuilder {
  public:
    [[nodiscard]] virtual BendRouteResult buildRoute(DocumentPtr, BendRouteRequest,
                                                     std::stop_token) const {
        return BendRouteError{BendRouteErrorCode::Unsupported, "Route builder unavailable"};
    }
    [[nodiscard]] virtual BendPreviewResult
    buildAssignedToolSetup(DocumentPtr document, std::size_t index, core::BendTarget target,
                           core::ToolAssignment assignment, std::stop_token stop) const {
        if (assignment.useDrawingGeometry || assignment.punchAssembly || assignment.dieAssembly)
            return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                    "DXF setup unavailable"};
        const auto tools = core::toolParameters(assignment);
        return tools ? buildToolSetup(document, index, target, *tools, stop)
                     : BendPreviewResult(BendPreviewError{
                           BendPreviewErrorCode::UnsupportedParameters, "Invalid assignment"});
    }

    virtual ~SingleBendPreviewBuilder() = default;
    [[nodiscard]] virtual BendPreviewResult
    buildAssignedToolMotion(DocumentPtr document, std::size_t index, core::BendTarget target,
                            core::ToolAssignment assignment, std::stop_token stop) const {
        if (assignment.useDrawingGeometry || assignment.punchAssembly || assignment.dieAssembly)
            return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                    "DXF tool contact unavailable"};
        const auto tools = core::toolParameters(assignment);
        return tools ? buildToolMotion(document, index, target, *tools, stop)
                     : BendPreviewResult(BendPreviewError{
                           BendPreviewErrorCode::UnsupportedParameters, "Invalid assignment"});
    }
    [[nodiscard]] virtual BendPreviewResult buildToolMotion(DocumentPtr, std::size_t,
                                                            core::BendTarget, core::PressTools,
                                                            std::stop_token) const {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                "Tool motion unavailable"};
    }
    [[nodiscard]] virtual BendPreviewResult buildToolSetup(DocumentPtr, std::size_t,
                                                           core::BendTarget, core::PressTools,
                                                           std::stop_token) const {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                "Tool setup unavailable"};
    }
    // Same source, confirmed zone and target. No writes to the source CAD topology.
    [[nodiscard]] virtual BendPreviewResult build(DocumentPtr document, std::size_t bendIndex,
                                                  core::BendTarget target, bool fixedSecond,
                                                  std::stop_token stop) const = 0;
};
} // namespace bending::application
