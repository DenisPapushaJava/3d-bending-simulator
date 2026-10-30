#pragma once
#include "bending/application/GeometryArtifactStore.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bending::cad {
class GeometryCacheScope;
class ToolGeometryCache {
  public:
    struct Statistics {
        std::uint64_t builds = 0, memoryHits = 0, diskHits = 0, writes = 0, writeFailures = 0;
        std::uint64_t rejected = 0;
        double buildMs = 0, loadMs = 0, saveMs = 0;
    };
    explicit ToolGeometryCache(std::shared_ptr<const application::GeometryArtifactStore> = {},
                               std::size_t memoryBytes = 64 * 1024 * 1024);
    ~ToolGeometryCache();
    [[nodiscard]] Statistics statistics() const;
    void clearMemory();

  private:
    friend class GeometryCacheScope;
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::cad
