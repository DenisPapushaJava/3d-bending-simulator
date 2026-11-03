#pragma once
#include "bending/core/ToolCatalog.h"
#include <filesystem>
#include <functional>
#include <string>
#include <variant>

namespace bending::application {
struct ToolLibraryData {
    core::ToolCatalog tools;
    core::ToolCatalog archived;
    bool operator==(const ToolLibraryData &) const = default;
};
struct ToolLibrarySnapshot {
    ToolLibraryData data;
    std::string revision;
    bool exists = true; // An intentionally empty file is different from first launch.
};
using ToolLibraryResult = std::variant<ToolLibrarySnapshot, std::string>;
struct ToolLibraryStorage {
    std::function<ToolLibraryResult(const std::filesystem::path &)> read;
    std::function<ToolLibraryResult(const std::filesystem::path &, const ToolLibraryData &,
                                    const std::string &expectedRevision)>
        write;
    explicit operator bool() const {
        return read && write;
    }
};
[[nodiscard]] bool validToolLibrary(const ToolLibraryData &);
// Single workshop owner. Mutations publish only after successful durable storage.
// Assignments are immutable copies, never live references into this library.
class ToolLibrary final {
  public:
    ToolLibrary(core::ToolCatalog initial, std::filesystem::path, ToolLibraryStorage,
                core::ToolCatalog starter = {});
    [[nodiscard]] const ToolLibraryData &data() const {
        return snapshot_.data;
    }
    [[nodiscard]] const std::string &error() const {
        return error_;
    }
    [[nodiscard]] std::string upsert(core::ToolPreset);
    [[nodiscard]] std::string replace(core::ToolCatalog);
    [[nodiscard]] std::string archive(const std::string &id);
    [[nodiscard]] std::string restore(const std::string &id);
    [[nodiscard]] std::string purge(const std::string &id);
    // Reference lengths only, never available stock. Geometry, not a mutable name/ID, binds them.
    [[nodiscard]] std::vector<core::ToolSegment> sectionTemplate(const core::ToolPreset &) const;
    // UI hint only: the profile and section rows still match the bundled starting set.
    [[nodiscard]] bool usesStarterSections(const core::ToolPreset &) const;

  private:
    const core::ToolPreset *starterProfile(const core::ToolPreset &) const;
    std::string commit(ToolLibraryData);
    ToolLibrarySnapshot snapshot_;
    std::filesystem::path path_;
    ToolLibraryStorage storage_;
    std::string error_;
    core::ToolCatalog starter_;
};
} // namespace bending::application
