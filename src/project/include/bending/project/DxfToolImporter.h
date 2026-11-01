#pragma once
#include "bending/application/ToolProfileImport.h"
namespace bending::project {
class DxfToolImporter final : public application::ToolProfileImporter {
  public:
    [[nodiscard]] application::ToolProfileResult run(const application::ToolProfileInput &,
                                                     std::stop_token) const override;
};
} // namespace bending::project
