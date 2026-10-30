#pragma once
#include "bending/cad/ToolGeometryCache.h"
#include <TopoDS_Shape.hxx>
#include <functional>
#include <stop_token>
#include <vector>

namespace bending::cad {
// Native adapter boundary: activate only on the CAD worker, never in viewer/GUI callbacks.
class GeometryCacheScope {
  public:
    GeometryCacheScope(const std::shared_ptr<ToolGeometryCache> &, std::stop_token);
    ~GeometryCacheScope();
    GeometryCacheScope(const GeometryCacheScope &) = delete;
    GeometryCacheScope &operator=(const GeometryCacheScope &) = delete;
    static TopoDS_Shape shape(const std::string &, const std::function<TopoDS_Shape()> &,
                              double deflection);
    static std::string descriptor(const TopoDS_Shape &);
    static bool enabled();

  private:
    struct KnownShape {
        std::string descriptor;
        TopoDS_Shape shape;
    };
    std::shared_ptr<ToolGeometryCache> owner_;
    std::vector<KnownShape> knownShapes_;
    std::vector<KnownShape> *previousKnown_;
    ToolGeometryCache::Impl *previous_;
    std::stop_token previousStop_;
    static thread_local ToolGeometryCache::Impl *active_;
    static thread_local std::stop_token stop_;
    static thread_local std::vector<KnownShape> *known_;
};
} // namespace bending::cad
