#pragma once
#include "bending/application/GeometryArtifactStore.h"
#include <atomic>
#include <filesystem>

namespace bending::project {
class GeometryCacheFiles final : public application::GeometryArtifactStore {
  public:
    explicit GeometryCacheFiles(std::filesystem::path directory);
    [[nodiscard]] std::optional<std::string> read(std::string_view descriptor) const override;
    [[nodiscard]] bool write(std::string_view descriptor, std::string_view bytes) const override;

  private:
    std::filesystem::path directory_;
    [[nodiscard]] bool cannotWrite() const;
    mutable std::atomic_flag warned_ = ATOMIC_FLAG_INIT;
};
} // namespace bending::project
