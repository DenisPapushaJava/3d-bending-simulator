#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace bending::application {
// Rebuildable binary artifacts, never project/catalogue source data. All calls run on the worker.
class GeometryArtifactStore {
  public:
    virtual ~GeometryArtifactStore() = default;
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view descriptor) const = 0;
    [[nodiscard]] virtual bool write(std::string_view descriptor, std::string_view bytes) const = 0;
};
} // namespace bending::application
