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
    [[nodiscard]] application::BendRouteResult buildRoute(application::DocumentPtr,
                                                          application::BendRouteRequest,
                                                          std::stop_token) const override;
    [[nodiscard]] application::BendPreviewResult
    buildAssignedToolSetup(application::DocumentPtr, std::size_t, core::BendTarget,
                           core::ToolAssignment, std::stop_token,
                           double maximumEndSupportLossMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult
    buildAssignedToolMotion(application::DocumentPtr, std::size_t, core::BendTarget,
                            core::ToolAssignment, std::stop_token,
                            double maximumEndSupportLossMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult
    buildToolMotion(application::DocumentPtr document, std::size_t bendIndex,
                    core::BendTarget target, core::PressTools tools, std::stop_token stop,
                    double maximumEndSupportLossMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult
    buildToolSetup(application::DocumentPtr document, std::size_t bendIndex,
                   core::BendTarget target, core::PressTools tools, std::stop_token stop,
                   double maximumEndSupportLossMm = 0) const override;
    [[nodiscard]] application::BendPreviewResult build(application::DocumentPtr document,
                                                       std::size_t bendIndex,
                                                       core::BendTarget target, bool fixedSecond,
                                                       std::stop_token stop) const override;

  private:
    struct RouteCache;
    mutable std::mutex routeCacheMutex_;
    mutable std::shared_ptr<RouteCache> routeCache_;
    std::shared_ptr<ToolGeometryCache> toolCache_;
};
} // namespace bending::cad
