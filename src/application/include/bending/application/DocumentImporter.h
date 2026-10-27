#pragma once

#include "bending/application/BendPreparation.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <variant>

namespace bending::application {
enum class ImportPhase { Waiting, Reading, Transferring, Validating, Analyzing, Preparing };
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
    InvalidProject,
    UnsupportedProjectVersion,
    UnexpectedFailure
};
struct ImportError {
    ImportErrorCode code;
    std::string detail;
    std::vector<ImportDiagnostic> diagnostics;
};
struct OpenedProject {
    DocumentPtr document;
    std::optional<double> manualThicknessMm;
    std::filesystem::path path;
    std::vector<BendPreparation> bends{};
};
[[nodiscard]] std::optional<std::string> projectSettingsError(const OpenedProject &project);
using ImportResult = std::variant<DocumentPtr, OpenedProject, ImportError>;
[[nodiscard]] inline DocumentPtr resultDocument(const ImportResult &result) {
    if (const auto *document = std::get_if<DocumentPtr>(&result)) {
        return *document;
    }
    if (const auto *project = std::get_if<OpenedProject>(&result)) {
        return project->document;
    }
    return {};
}
using ImportProgress = std::function<void(ImportPhase)>;

class DocumentImporter {
  public:
    virtual ~DocumentImporter() = default;
    [[nodiscard]] virtual ImportResult load(const std::filesystem::path &path, std::stop_token stop,
                                            const ImportProgress &progress) const = 0;
};
} // namespace bending::application
