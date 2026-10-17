#pragma once

#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
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
};
using BendPreviewPtr = std::shared_ptr<const SingleBendPreview>;
enum class BendPreviewErrorCode {
    UnsupportedGeometry,
    UnsupportedParameters,
    TargetMismatch,
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
    // Same source, confirmed zone and target. No writes to the source CAD topology.
    [[nodiscard]] virtual BendPreviewResult build(DocumentPtr document, std::size_t bendIndex,
                                                  core::BendTarget target, bool fixedSecond,
                                                  std::stop_token stop) const = 0;
};
} // namespace bending::application
