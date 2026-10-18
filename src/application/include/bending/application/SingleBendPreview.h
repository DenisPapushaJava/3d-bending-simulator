#pragma once

#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/SingleBend.h"

#include <stop_token>
#include <variant>

namespace bending::application {
class SingleBendPreview {
  public:
    virtual ~SingleBendPreview() = default;
    [[nodiscard]] virtual const simulation::SingleBend &model() const noexcept = 0;
    [[nodiscard]] virtual std::size_t frameCount() const noexcept = 0;
    [[nodiscard]] virtual BoundsMm bounds() const noexcept = 0;
    // Non-null only for a static, checked initial tool setup (one frame).
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
    Cancelled,
    Failed
};
struct BendPreviewError {
    BendPreviewErrorCode code;
    std::string detail;
};
using BendPreviewResult = std::variant<BendPreviewPtr, BendPreviewError>;
class SingleBendPreviewBuilder {
  public:
    virtual ~SingleBendPreviewBuilder() = default;
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
