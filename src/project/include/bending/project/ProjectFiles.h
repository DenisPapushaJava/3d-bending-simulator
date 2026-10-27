#pragma once

#include "bending/application/DocumentImporter.h"
#include "bending/application/ProjectWriter.h"
#include "bending/cad/StepImporter.h"

namespace bending::project {
// Qt Core JSON/atomic-file adapter, composed in app. Owns no GUI or mutable CAD handles.
class ProjectFiles final : public application::DocumentImporter, public application::ProjectWriter {
  public:
    [[nodiscard]] application::ImportResult
    load(const std::filesystem::path &path, std::stop_token stop,
         const application::ImportProgress &progress) const override;
    [[nodiscard]] std::optional<std::string>
    save(const std::filesystem::path &path, const application::Document &document,
         std::optional<double> manualThicknessMm,
         const std::vector<application::BendPreparation> &bends,
         std::stop_token stop) const override;

  private:
    cad::StepImporter importer_;
};
} // namespace bending::project
