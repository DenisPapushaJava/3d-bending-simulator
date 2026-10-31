#pragma once
#include "bending/application/ToolLibrary.h"
#include "bending/core/ToolCatalog.h"
#include <filesystem>
#include <variant>

namespace bending::project {
using ToolCatalogResult = std::variant<core::ToolCatalog, std::string>;
// Small bounded JSON catalogues. No executable extensions or remote loading.
[[nodiscard]] ToolCatalogResult readToolCatalog(const std::filesystem::path &path);
// Atomic local library snapshot. Does not overwrite source DXF or the imported catalogue.
[[nodiscard]] std::optional<std::string> writeToolCatalog(const std::filesystem::path &,
                                                          const core::ToolCatalog &);
// Versioned workshop + recoverable archive, guarded against concurrent writers.
[[nodiscard]] application::ToolLibraryResult readToolLibrary(const std::filesystem::path &);
[[nodiscard]] application::ToolLibraryResult writeToolLibrary(const std::filesystem::path &,
                                                              const application::ToolLibraryData &,
                                                              const std::string &expectedRevision);
} // namespace bending::project
