#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <stop_token>

namespace bending::cad {
// Native CAD integration; callers must use the shared CAD worker.
// Worker-owned solids. The built-in revision uses our approximate teaching
// profiles, not imported proprietary contours or certified machine dimensions.
class MachineAssembly {
  public:
    MachineAssembly(const core::PressTools &, const std::array<TopoDS_Shape, 2> &, std::stop_token);
    [[nodiscard]] std::vector<MachineDisplayComponent> at(double punchTranslationY) const;
    [[nodiscard]] static std::optional<application::MachineCollision>
    check(const PreviewFrame &, double toleranceMm, std::stop_token);

  private:
    std::vector<MachineDisplayComponent> components_;
};
} // namespace bending::cad
