#include "bending/application/DocumentSession.h"

#include <utility>

namespace bending::application {
ImportRequest DocumentSession::beginImport(std::filesystem::path source) {
    active_ = ImportRequest{++nextRequest_, revision_, std::move(source)};
    error_.reset();
    phase_ = ImportPhase::Waiting;
    return *active_;
}

bool DocumentSession::isCurrent(const ImportRequest &request) const noexcept {
    return active_ && active_->id == request.id && request.baseRevision == revision_ &&
           active_->baseRevision == request.baseRevision;
}

bool DocumentSession::updateProgress(const ImportRequest &request, ImportPhase phase) {
    if (!isCurrent(request)) {
        return false;
    }
    phase_ = phase;
    return true;
}

bool DocumentSession::complete(const ImportRequest &request, ImportResult result) {
    if (!isCurrent(request)) {
        return false;
    }
    active_.reset();
    if (auto document = resultDocument(result)) {
        const auto *project = std::get_if<OpenedProject>(&result);
        if (project && project->manualThicknessMm) {
            const auto validation =
                validateThickness(*project->manualThicknessMm, document->metadata().thickness);
            if (validation == ThicknessValidation::Invalid ||
                validation == ThicknessValidation::ConflictsWithGeometry) {
                error_ = ImportError{
                    ImportErrorCode::InvalidProject, "Saved thickness conflicts with geometry", {}};
                return true;
            }
        }
        document_ = std::move(document);
        ++revision_;
        ++editRevision_;
        manualThickness_ = project ? project->manualThicknessMm : std::nullopt;
        projectPath_ = project ? project->path : std::filesystem::path{};
        modified_ = false;
        error_.reset();
    } else if (const auto *error = std::get_if<ImportError>(&result)) {
        if (error->code != ImportErrorCode::Cancelled) {
            error_ = *error;
        }
    } else {
        error_ = ImportError{
            ImportErrorCode::UnexpectedFailure, "Importer returned a null document", {}};
    }
    return true;
}

void DocumentSession::cancel() {
    active_.reset();
    error_.reset();
}

void DocumentSession::close() {
    cancel();
    document_.reset();
    ++revision_;
    ++editRevision_;
    manualThickness_.reset();
    projectPath_.clear();
    modified_ = false;
}

ThicknessValidation DocumentSession::setManualThickness(double valueMm) {
    if (!document_ || active_) {
        return ThicknessValidation::Invalid;
    }
    const auto validation = validateThickness(valueMm, document_->metadata().thickness);
    if (validation != ThicknessValidation::Invalid &&
        validation != ThicknessValidation::ConflictsWithGeometry && manualThickness_ != valueMm) {
        manualThickness_ = valueMm;
        modified_ = true;
        ++editRevision_;
    }
    return validation;
}
void DocumentSession::useAutomaticThickness() {
    if (document_ && !active_ && manualThickness_) {
        manualThickness_.reset();
        modified_ = true;
        ++editRevision_;
    }
}
bool DocumentSession::markSaved(std::uint64_t editRevision, std::filesystem::path path) {
    if (!document_ || editRevision != editRevision_) {
        return false;
    }
    projectPath_ = std::move(path);
    modified_ = false;
    return true;
}
} // namespace bending::application
