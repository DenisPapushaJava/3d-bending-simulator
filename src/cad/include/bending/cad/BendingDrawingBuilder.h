#pragma once
#include "bending/application/BendingDrawing.h"

namespace bending::cad {
class BendingDrawingBuilder final : public application::BendingDrawingBuilder {
  public:
    [[nodiscard]] application::DrawingResult build(application::DocumentPtr document,
                                                   application::DrawingRequest request,
                                                   std::stop_token stop) const override;
};
} // namespace bending::cad
