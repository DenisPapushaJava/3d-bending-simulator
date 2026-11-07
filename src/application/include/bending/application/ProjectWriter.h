#pragma once

#include "bending/application/BendPreparation.h"
#include "bending/application/ProjectDrawingSettings.h"
#include "bending/core/UnfoldRule.h"

#include <optional>
#include <stop_token>

namespace bending::application {
class ProjectWriter {
  public:
    virtual ~ProjectWriter() = default;
    // A portable snapshot; an empty result means an atomic save succeeded.
    [[nodiscard]] virtual std::optional<std::string>
    save(const std::filesystem::path &path, const Document &document,
         std::optional<double> manualThicknessMm, const std::vector<BendPreparation> &bends,
         std::stop_token stop, core::UnfoldSettings unfold = {},
         const std::optional<ProjectDrawingSettings> &drawing = {}) const = 0;
};
} // namespace bending::application
