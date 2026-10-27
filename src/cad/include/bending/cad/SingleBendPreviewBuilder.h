#pragma once

#include "bending/application/SingleBendPreview.h"

namespace bending::cad {
class SingleBendPreviewBuilder final : public application::SingleBendPreviewBuilder {
  public:
    [[nodiscard]] application::BendPreviewResult
    buildToolMotion(application::DocumentPtr document, std::size_t bendIndex,
                    core::BendTarget target, core::PressTools tools,
                    std::stop_token stop) const override;
    [[nodiscard]] application::BendPreviewResult
    buildToolSetup(application::DocumentPtr document, std::size_t bendIndex,
                   core::BendTarget target, core::PressTools tools,
                   std::stop_token stop) const override;
    [[nodiscard]] application::BendPreviewResult build(application::DocumentPtr document,
                                                       std::size_t bendIndex,
                                                       core::BendTarget target, bool fixedSecond,
                                                       std::stop_token stop) const override;
};
} // namespace bending::cad
