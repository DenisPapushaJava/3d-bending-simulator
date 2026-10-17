#pragma once

#include "bending/application/Document.h"

#include <functional>
#include <memory>
#include <stop_token>
#include <variant>

namespace bending::application {
enum class ImportPhase { Waiting, Reading, Transferring, Validating, Preparing };
enum class ImportErrorCode {
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
    KernelFailure,
    Cancelled,
    PreparationFailed,
    PresentationFailed,
    UnexpectedFailure
};
struct ImportError {
    ImportErrorCode code;
    std::string detail;
    std::vector<ImportDiagnostic> diagnostics;
};
using ImportResult = std::variant<DocumentPtr, ImportError>;
using ImportProgress = std::function<void(ImportPhase)>;

class DocumentImporter {
  public:
    virtual ~DocumentImporter() = default;
    [[nodiscard]] virtual ImportResult load(const std::filesystem::path &path, std::stop_token stop,
                                            const ImportProgress &progress) const = 0;
};
} // namespace bending::application
