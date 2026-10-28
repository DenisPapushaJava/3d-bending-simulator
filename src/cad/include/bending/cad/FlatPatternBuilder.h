#pragma once
#include "bending/application/FlatPattern.h"

namespace bending::cad {
class FlatPatternBuilder final : public application::FlatPatternBuilder {
  public:
    [[nodiscard]] application::FlatPatternResult build(application::DocumentPtr,
                                                       application::FlatPatternRequest,
                                                       std::stop_token) const override;
};
} // namespace bending::cad
