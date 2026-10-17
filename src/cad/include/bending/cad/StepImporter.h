#pragma once

#include "bending/cad/CadDocument.h"

#include <variant>

namespace bending::cad {
enum class StepImportErrorCode {
    UnsupportedFormat,
    FileNotFound,
    ReadFailed,
    EmptyFile,
    InvalidStep,
    NoGeometry,
    UnsupportedUnits,
    UnsupportedComposition,
    TransferFailed,
    InvalidGeometry,
    KernelFailure
};

struct StepImportError {
    StepImportErrorCode code;
    std::string detail;
    std::vector<ImportDiagnostic> diagnostics;
};

// Success always contains a non-null document. A failure never changes an existing document.
using StepImportResult = std::variant<std::unique_ptr<const CadDocument>, StepImportError>;

class StepImporter final {
  public:
    // Reads a single-file .step/.stp. Calls are serialized at the OCCT boundary (7.6
    // compatibility). This is synchronous; the application layer must schedule it outside the GUI
    // thread.
    [[nodiscard]] StepImportResult read(const std::filesystem::path &path) const;
};
} // namespace bending::cad
