#pragma once

#include "bending/application/SingleBendPreview.h"
#include "bending/cad/ToolGeometryCache.h"
#include <mutex>

namespace bending::cad {
class SingleBendPreviewBuilder final : public application::SingleBendPreviewBuilder {
  public:
    explicit SingleBendPreviewBuilder(
        std::shared_ptr<ToolGeometryCache> = std::make_shared<ToolGeometryCache>());
    ~SingleBendPreviewBuilder() override;
    void discardRouteCache() const override;
    [[nodiscard]] application::RouteDraftResult buildRouteDraft(application::DocumentPtr,
                                                                application::RouteDraftRequest,
                                                                std::stop_token) const override;
    [[nodiscard]] application::RouteDraftResult
    buildRouteDraftWithPose(application::DocumentPtr, application::RouteDraftRequest,
                            std::stop_token,
                            const application::RouteDraftPoseObserver &) const override;
    [[nodiscard]] application::BendRouteResult buildRoute(application::DocumentPtr,
                                                          application::BendRouteRequest,
                                                          std::stop_token) const override;
    [[nodiscard]] application::BendPreviewResult buildAssignedToolSetup(
        application::DocumentPtr, std::size_t, core::BendTarget, core::ToolAssignment,
        std::stop_token, std::optional<double> maximumEndSupportLossMm = 0,
        bool workpieceReversed = false, double workpieceOffsetMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult buildAssignedToolMotion(
        application::DocumentPtr, std::size_t, core::BendTarget, core::ToolAssignment,
        std::stop_token, std::optional<double> maximumEndSupportLossMm = 0,
        bool workpieceReversed = false, double workpieceOffsetMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult
    buildToolMotion(application::DocumentPtr document, std::size_t bendIndex,
                    core::BendTarget target, core::PressTools tools, std::stop_token stop,
                    std::optional<double> maximumEndSupportLossMm = 0,
                    bool workpieceReversed = false, double workpieceOffsetMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult
    buildToolSetup(application::DocumentPtr document, std::size_t bendIndex,
                   core::BendTarget target, core::PressTools tools, std::stop_token stop,
                   std::optional<double> maximumEndSupportLossMm = 0,
                   bool workpieceReversed = false, double workpieceOffsetMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult build(application::DocumentPtr document,
                                                       std::size_t bendIndex,
                                                       core::BendTarget target, bool fixedSecond,
                                                       std::stop_token stop) const override;

  private:
    application::BendRouteResult buildRouteImpl(application::DocumentPtr,
                                                application::BendRouteRequest, std::stop_token,
                                                unsigned pairedContinuationTimeMs) const;
    application::BendPreviewResult cachedOperation(application::DocumentPtr, std::size_t,
                                                   core::BendTarget, bool reversed, std::stop_token,
                                                   core::PressTools, bool motion,
                                                   std::optional<core::ToolAssignment>,
                                                   std::optional<double> maximumEndSupportLossMm,
                                                   double workpieceOffsetMm) const;
    struct SingleCache;
    mutable std::shared_ptr<SingleCache> singleCache_;
    struct RouteCache;
    mutable std::mutex routeCacheMutex_;
    mutable std::shared_ptr<RouteCache> routeCache_;
    std::shared_ptr<ToolGeometryCache> toolCache_;
};
} // namespace bending::cad
