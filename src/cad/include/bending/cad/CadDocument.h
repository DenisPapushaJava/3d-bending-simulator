#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bending::cad {
enum class ImportStage { Read, Transfer };
enum class DiagnosticSeverity { Warning, Error };

struct ImportDiagnostic {
    ImportStage stage;
    DiagnosticSeverity severity;
    int entityNumber;
    std::string message;
};

struct PointMm {
    double x;
    double y;
    double z;
};

struct BoundsMm {
    PointMm minimum;
    PointMm maximum;
};

struct DocumentMetadata {
    std::filesystem::path sourcePath;
    std::vector<std::string> sourceLengthUnits;
    std::string kernelVersion;
    BoundsMm bounds;
    double volumeMm3;
    double maximumToleranceMm;
    std::size_t faceCount;
    std::vector<ImportDiagnostic> diagnostics;
};

class StepImporter;

// One validated solid in millimetres. Only the importer can create a document.
// The geometry is owned here; copying a mutable OCCT handle is not part of this API.
class CadDocument final {
  public:
    ~CadDocument();
    CadDocument(const CadDocument &) = delete;
    CadDocument &operator=(const CadDocument &) = delete;

    [[nodiscard]] const DocumentMetadata &metadata() const noexcept;

  private:
    struct Impl;
    explicit CadDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class StepImporter;
};
} // namespace bending::cad
