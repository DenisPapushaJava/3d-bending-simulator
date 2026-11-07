#pragma once

#include "bending/application/DocumentImporter.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bending::application {
struct ImportRequest {
    std::uint64_t id;
    std::uint64_t baseRevision;
    std::filesystem::path source;
};

struct BendToolUpdate {
    std::size_t bendIndex;
    std::optional<core::PressTools> tools;
    std::optional<core::ToolAssignment> assignment;
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
    [[nodiscard]] ThicknessValidation setManualThickness(double valueMm);
    void useAutomaticThickness();
    bool setUnfoldSettings(core::UnfoldSettings);
    // One revision-checked transaction for the part-input form. nullopt preserves auto origin.
    bool setPartInputs(std::uint64_t revision, std::uint64_t editRevision,
                       std::optional<double> manualThicknessMm, core::UnfoldSettings);
    [[nodiscard]] bool hasPartInputs() const;
    [[nodiscard]] const core::UnfoldSettings &unfoldSettings() const {
        return unfold_;
    }
    bool setBendWorkpieceOrientation(std::size_t recognizedIndex, bool reversed);
    bool setBendWorkpieceOffset(std::size_t recognizedIndex, double offsetMm);
    bool setBendReview(std::size_t recognizedIndex, BendReview review);
    bool setBendTools(std::size_t recognizedIndex, std::optional<core::PressTools> tools);
    bool setBendToolAssignment(std::size_t recognizedIndex, core::ToolAssignment assignment);
    // Validate the entire batch before publishing it; a change advances one edit revision.
    bool setBendToolAssignments(const std::vector<BendToolUpdate> &updates);
    [[nodiscard]] std::optional<core::BendTarget> bendTarget(std::size_t recognizedIndex) const;
    bool setBendTarget(std::size_t recognizedIndex, std::optional<core::BendTarget> target);
    [[nodiscard]] std::vector<std::size_t> orderedBends() const;
    // The existing preparation array carries the preferred order, not a validation result.
    bool setBendOrder(const std::vector<std::string> &ids);
    [[nodiscard]] const std::vector<BendPreparation> &bends() const {
        return bends_;
    }
    [[nodiscard]] const BendBindings &bendBindings() const {
        return bendBindings_;
    }
    [[nodiscard]] const BendPreparation *bend(std::size_t recognizedIndex) const;
    bool setDrawingSettings(std::uint64_t revision, ProjectDrawingSettings);
    [[nodiscard]] const std::optional<ProjectDrawingSettings> &drawingSettings() const {
        return drawing_;
    }
    [[nodiscard]] std::uint64_t drawingRevision() const {
        return drawingRevision_;
    }
    bool markSaved(std::uint64_t editRevision, std::filesystem::path path);
    [[nodiscard]] const std::optional<double> &manualThicknessMm() const {
        return manualThickness_;
    }
    [[nodiscard]] const std::filesystem::path &projectPath() const {
        return projectPath_;
    }
    [[nodiscard]] bool modified() const {
        return modified_;
    }
    [[nodiscard]] std::uint64_t editRevision() const {
        return editRevision_;
    }

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
    std::optional<double> manualThickness_;
    std::filesystem::path projectPath_;
    std::uint64_t editRevision_ = 0;
    bool modified_ = false;
    std::vector<BendPreparation> bends_;
    BendBindings bendBindings_;
    core::UnfoldSettings unfold_;
    std::optional<ProjectDrawingSettings> drawing_;
    std::uint64_t drawingRevision_ = 0;
};
} // namespace bending::application
