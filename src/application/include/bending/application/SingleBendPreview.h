#pragma once

#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/PressMotion.h"
#include "bending/simulation/SingleBend.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <variant>

namespace bending::application {
enum class BendPreviewMode { Kinematic, ToolSetup, ToolMotion };
class SingleBendPreview {
  public:
    virtual ~SingleBendPreview() = default;
    [[nodiscard]] virtual const simulation::PressMotion *pressMotion() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const simulation::PressState *pressState(std::size_t) const noexcept {
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
    UnsupportedParameters,
    TargetMismatch,
    ToolTooShort,
    MissingLeftSupport,
    MissingRightSupport,
    ToolAngleConflict,
    NoseTooLarge,
    SupportNotProven,
    ToolIntersection,
    ContactLost,
    Cancelled,
    Failed
};
struct BendPreviewError {
    BendPreviewErrorCode code;
    std::string detail;
    std::optional<std::size_t> frameIndex = std::nullopt;
};
using BendPreviewResult = std::variant<BendPreviewPtr, BendPreviewError>;
class SingleBendPreviewBuilder {
  public:
    virtual ~SingleBendPreviewBuilder() = default;
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
