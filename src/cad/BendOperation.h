#pragma once
#include "bending/application/SingleBendPreview.h"
#include <functional>
#include <gp_Dir.hxx>
#include <span>
#include <utility>

namespace bending::cad {
class CadDocument;
struct PreparedBendWorkpiece;
// Bounded to four source/state/zone/side snapshots by prepareBendOperation.
// Owned and serialized by the route worker; must outlive its prepared operations.
struct BendWorkpieceReuse {
    std::vector<std::shared_ptr<const PreparedBendWorkpiece>> entries;
    std::size_t built = 0, reused = 0, checked = 0, reusedChecks = 0;
};
gp_Dir normalBendAxis(const CadDocument &, std::size_t);
// Worker-local immutable preparation for one exact operation/side/tool setup.
// Only axial placement may vary. Installation frames/contact verdicts are independent;
// D209 may separately reuse successful intrinsic checks of the exact sheet geometry.
class PreparedBendOperation {
  public:
    using Evaluate = std::function<application::BendPreviewResult(
        double, std::stop_token, bool, application::BackgaugeSearchStatistics *)>;
    explicit PreparedBendOperation(Evaluate evaluate) : evaluate_(std::move(evaluate)) {}
    PreparedBendOperation(const PreparedBendOperation &) = delete;
    PreparedBendOperation &operator=(const PreparedBendOperation &) = delete;
    application::BendPreviewResult
    at(double offsetMm, std::stop_token, bool prepareDiagnostic = true,
       application::BackgaugeSearchStatistics *statistics = nullptr) const;

  private:
    Evaluate evaluate_;
};
using PreparedBendOperationPtr = std::shared_ptr<const PreparedBendOperation>;
using PreparedBendOperationResult =
    std::variant<PreparedBendOperationPtr, application::BendPreviewError>;
PreparedBendOperationResult prepareBendOperation(
    application::DocumentPtr, std::size_t, core::BendTarget, bool fixedSecond, std::stop_token,
    std::optional<core::PressTools>, bool withMotion = false,
    std::optional<core::ToolAssignment> = {}, bool allowPartialEndSupport = false,
    std::optional<double> maximumEndSupportLossMm = {}, std::optional<gp_Dir> referenceAxis = {},
    std::span<const std::size_t> jointBends = {}, BendWorkpieceReuse *workpieceReuse = nullptr,
    const application::FlatPatternPtr &verifiedFlat = {}, bool manualExtraction = false);
// Shared CAD transition for an isolated operation and a route's intermediate state.
application::BendPreviewResult
buildBendOperation(application::DocumentPtr, std::size_t, core::BendTarget, bool fixedSecond,
                   std::stop_token, std::optional<core::PressTools>, bool withMotion = false,
                   std::optional<core::ToolAssignment> = {}, bool allowPartialEndSupport = false,
                   std::optional<double> maximumEndSupportLossMm = {},
                   std::optional<gp_Dir> referenceAxis = {}, bool prepareDiagnostic = true,
                   application::BackgaugeSearchStatistics *statistics = nullptr,
                   std::span<const std::size_t> jointBends = {}, double workpieceOffsetMm = 0);
} // namespace bending::cad
