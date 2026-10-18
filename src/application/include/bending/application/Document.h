#pragma once

#include "bending/application/Thickness.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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
// Measurements of local cylindrical bend zones, not a manufacturing operation or a
// proof that the entire solid is a sheet-metal part. Face indices are revision-scoped.
enum class BendRecognitionStatus { NotAnalyzed, Completed, Unsupported, LimitReached, Failed };
struct RecognizedBend {
    PointMm axisStart;
    PointMm axisEnd;
    double angleRadians;
    double innerRadiusMm;
    double thicknessMm;
    double lengthMm;
    std::size_t innerFace;
    std::size_t outerFace;
    std::array<std::size_t, 4> flangeFaces;
    bool trimmedEnds = false;
    // Conservative common axial span present at every angle on both cylinders.
    double minimumStraightLengthMm = 0.0;
};
// A paired cylindrical sector interrupted by enclosed contours (hole or pocket).
// Candidate measurements describe its outer boundary, not a supported bend.
// All face references are scoped to this immutable document revision.
struct BendInteriorContourDiagnostic {
    RecognizedBend candidate;
    std::vector<std::size_t> boundaryFaces;
};
struct BendAnalysis {
    BendRecognitionStatus status = BendRecognitionStatus::NotAnalyzed;
    double lengthToleranceMm = 1.0e-5;
    double angleToleranceRadians = 1.0e-5;
    std::size_t unrecognizedCylindricalFaces = 0;
    std::vector<RecognizedBend> bends;
    std::vector<BendInteriorContourDiagnostic> interiorContourDiagnostics;
};
enum class SurfaceKind { Plane, Cylinder, Cone, Sphere, Torus, Bezier, BSpline, Other };
struct FaceInfo {
    std::size_t index; // One-based, scoped to this document revision.
    double areaMm2;
    SurfaceKind surface;
};
enum class ClosedCutoutStatus { None, Filled, Unsupported, Failed };
// Result for a separate approximation; IDs always refer to original CAD faces.
struct ClosedCutouts {
    ClosedCutoutStatus status = ClosedCutoutStatus::None;
    std::vector<std::size_t> boundaryFaces;
    double addedVolumeMm3 = 0.0;
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
    ThicknessAnalysis thickness{};
    BendAnalysis bendAnalysis{};
    ClosedCutouts closedCutouts{};
};

// Immutable snapshot shared by the session and its presenter after worker preparation ends.
class Document {
  public:
    virtual ~Document() = default;
    [[nodiscard]] virtual const DocumentMetadata &metadata() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<FaceInfo> &faces() const noexcept = 0;
    // Original STEP bytes captured during import, never re-read from a mutable source at save.
    [[nodiscard]] virtual std::string_view sourceStep() const noexcept {
        return {};
    }
};
using DocumentPtr = std::shared_ptr<const Document>;
} // namespace bending::application
