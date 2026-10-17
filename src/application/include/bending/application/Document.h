#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bending::application {
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
enum class SurfaceKind { Plane, Cylinder, Cone, Sphere, Torus, Bezier, BSpline, Other };
struct FaceInfo {
    std::size_t index; // One-based, scoped to this document revision.
    double areaMm2;
    SurfaceKind surface;
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
    double meshDeflectionMm = 0.0;
    double meshAngleRadians = 0.0;
};

// Immutable snapshot shared by the session and its presenter after worker preparation ends.
class Document {
  public:
    virtual ~Document() = default;
    [[nodiscard]] virtual const DocumentMetadata &metadata() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<FaceInfo> &faces() const noexcept = 0;
};
using DocumentPtr = std::shared_ptr<const Document>;
} // namespace bending::application
