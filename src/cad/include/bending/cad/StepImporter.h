#pragma once

#include "bending/application/DocumentImporter.h"
#include "bending/cad/CadDocument.h"

#include <variant>

namespace bending::cad {
using StepImportErrorCode = application::ImportErrorCode;
using StepImportError = application::ImportError;

// Success always contains a non-null document. A failure never changes an existing document.
using StepImportResult = std::variant<std::unique_ptr<const CadDocument>, StepImportError>;

class StepImporter final : public application::DocumentImporter {
  public:
    // Reads a single-file, single-part .step/.stp; assemblies are unsupported.
    // Calls are serialized at the OCCT boundary (7.6 compatibility).
    // This is synchronous; the application layer must schedule it outside the GUI thread.
    [[nodiscard]] StepImportResult read(const std::filesystem::path &path,
                                        std::stop_token stop = {},
                                        const application::ImportProgress &progress = {}) const;
    [[nodiscard]] application::ImportResult
    load(const std::filesystem::path &path, std::stop_token stop,
         const application::ImportProgress &progress) const override;
};
} // namespace bending::cad
