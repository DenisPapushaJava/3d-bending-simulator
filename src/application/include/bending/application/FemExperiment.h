#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace bending::application {
// Deliberately bounded synthetic experiment; no CAD shape or manufacturing operation.
enum class FemHole { Center, Offset6Mm, None };
enum class FemMesh { Coarse, Medium, Fine };
struct FemParameters {
    FemHole hole = FemHole::Center;
    FemMesh mesh = FemMesh::Medium;
    bool unload = true;
    bool operator==(const FemParameters &) const = default;
};
struct FemRequest {
    FemParameters parameters;
    // Human-readable local provenance only. Never used to reconstruct a blank from STEP.
    std::string context;
};
struct FemResult {
    std::filesystem::path directory;
    std::filesystem::path animation;
    double formedForceN;
    double formedAngleDeg;
    std::optional<double> unloadedAngleDeg;
};
enum class FemOutcomeStatus { Completed, Failed, Cancelled, StopFailed };
struct FemOutcome {
    FemOutcomeStatus status;
    std::optional<FemResult> result;
    std::string detail;
};
class FemRunner {
  public:
    using Progress = std::function<void(std::string)>;
    using Completion = std::function<void(FemOutcome)>;
    virtual ~FemRunner() = default;
    // Owner-thread API; callbacks also run there, completion is always asynchronous.
    // Exactly one active job. Cancellation completes only after external work stops.
    virtual void start(FemRequest request, Progress progress, Completion completion) = 0;
    virtual void cancel() = 0;
};
} // namespace bending::application
