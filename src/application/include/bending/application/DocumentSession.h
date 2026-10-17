#pragma once

#include "bending/application/DocumentImporter.h"

#include <cstdint>
#include <optional>

namespace bending::application {
struct ImportRequest {
    std::uint64_t id;
    std::uint64_t baseRevision;
    std::filesystem::path source;
};

// GUI-independent state machine. Its owner serializes calls; no Qt, SDK or worker ownership here.
class DocumentSession final {
  public:
    [[nodiscard]] ImportRequest beginImport(std::filesystem::path source);
    [[nodiscard]] bool isCurrent(const ImportRequest &request) const noexcept;
    bool updateProgress(const ImportRequest &request, ImportPhase phase);
    bool complete(const ImportRequest &request, ImportResult result);
    void cancel();
    void close();

    [[nodiscard]] const DocumentPtr &document() const noexcept {
        return document_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] const std::optional<ImportRequest> &activeRequest() const noexcept {
        return active_;
    }
    [[nodiscard]] ImportPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] const std::optional<ImportError> &error() const noexcept {
        return error_;
    }

  private:
    std::uint64_t nextRequest_ = 0;
    std::uint64_t revision_ = 0;
    DocumentPtr document_;
    std::optional<ImportRequest> active_;
    std::optional<ImportError> error_;
    ImportPhase phase_ = ImportPhase::Waiting;
};
} // namespace bending::application
