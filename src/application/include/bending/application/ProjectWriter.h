#pragma once

#include "bending/application/Document.h"

#include <optional>
#include <stop_token>

namespace bending::application {
class ProjectWriter {
  public:
    virtual ~ProjectWriter() = default;
    // A portable snapshot; an empty result means an atomic save succeeded.
    [[nodiscard]] virtual std::optional<std::string> save(const std::filesystem::path &path,
                                                          const Document &document,
                                                          std::optional<double> manualThicknessMm,
                                                          std::stop_token stop) const = 0;
};
} // namespace bending::application
