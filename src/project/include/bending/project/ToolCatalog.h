#pragma once
#include "bending/core/ToolCatalog.h"
#include <filesystem>
#include <variant>

namespace bending::project {
using ToolCatalogResult = std::variant<core::ToolCatalog, std::string>;
// Small bounded JSON catalogues. No executable extensions or remote loading.
[[nodiscard]] ToolCatalogResult readToolCatalog(const std::filesystem::path &path);
[[nodiscard]] ToolCatalogResult builtInToolCatalog();
} // namespace bending::project
