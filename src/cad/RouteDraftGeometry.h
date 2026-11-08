#pragma once
#include "bending/application/BendRoute.h"
#include "bending/application/FlatPattern.h"
namespace bending::cad {
// One classified source skin per imported document/kinematic thickness.
// Successful fallback bindings are retained under the route cache's worker lock.
class RouteDraftBasis {
  public:
    RouteDraftBasis(application::DocumentPtr, application::FlatPatternPtr, std::stop_token);
    ~RouteDraftBasis();
    application::RouteDraftPtr build(application::RouteDraftRequest, bool reused,
                                     std::stop_token) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::cad
