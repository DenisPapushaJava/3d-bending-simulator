#include "bending/application/DocumentSession.h"

#include <utility>

namespace bending::application {
std::optional<std::string> projectSettingsError(const OpenedProject &project) {
    if (!project.document || !validBendPreparations(project.bends)) {
        return "Invalid saved bend preparations";
    }
    if (project.manualThicknessMm) {
        const auto validation =
            validateThickness(*project.manualThicknessMm, project.document->metadata().thickness);
        if (validation == ThicknessValidation::Invalid ||
            validation == ThicknessValidation::ConflictsWithGeometry) {
            return "Saved thickness conflicts with geometry";
        }
    }
    return {};
}
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
        if (project) {
            if (const auto error = projectSettingsError(*project)) {
                error_ = ImportError{ImportErrorCode::InvalidProject, *error, {}};
                return true;
            }
        }
        auto bends = project ? project->bends : std::vector<BendPreparation>{};
        auto bindings = restoreBendPreparations(document->metadata().bendAnalysis, bends);
        document_ = std::move(document);
        bends_ = std::move(bends);
        bendBindings_ = std::move(bindings);
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
    bends_.clear();
    bendBindings_ = {};
}

const BendPreparation *DocumentSession::bend(std::size_t recognizedIndex) const {
    if (recognizedIndex >= bendBindings_.byRecognizedBend.size() ||
        !bendBindings_.byRecognizedBend[recognizedIndex]) {
        return nullptr;
    }
    return &bends_[*bendBindings_.byRecognizedBend[recognizedIndex]];
}
bool DocumentSession::setBendReview(std::size_t recognizedIndex, BendReview review) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry ||
        (review != BendReview::Pending && review != BendReview::Confirmed &&
         review != BendReview::Excluded && review != BendReview::Automatic)) {
        return false;
    }
    if (entry->review != review) {
        bends_[*bendBindings_.byRecognizedBend[recognizedIndex]].review = review;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}
bool DocumentSession::setBendTarget(std::size_t recognizedIndex,
                                    std::optional<core::BendTarget> target) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry || !included(entry->review) ||
        (target && !core::validBendTarget(*target))) {
        return false;
    }
    if (entry->target != target) {
        bends_[*bendBindings_.byRecognizedBend[recognizedIndex]].target = target;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

bool DocumentSession::setBendTools(std::size_t recognizedIndex,
                                   std::optional<core::PressTools> tools) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry || !included(entry->review) ||
        (tools && !core::validPressTools(*tools))) {
        return false;
    }
    if (entry->tools != tools || entry->catalogTools) {
        auto &changed = bends_[*bendBindings_.byRecognizedBend[recognizedIndex]];
        changed.tools = tools;
        changed.catalogTools.reset();
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

std::optional<core::BendTarget> DocumentSession::bendTarget(std::size_t index) const {
    const auto *entry = bend(index);
    if (!document_ || !entry || !included(entry->review) ||
        index >= document_->metadata().bendAnalysis.bends.size()) {
        return {};
    }
    if (entry->target) {
        return entry->target;
    }
    const auto &bend = document_->metadata().bendAnalysis.bends[index];
    const core::BendTarget target{bend.angleRadians, bend.innerRadiusMm};
    return core::validBendTarget(target) ? std::optional{target} : std::nullopt;
}
bool DocumentSession::setBendToolAssignment(std::size_t index, core::ToolAssignment assignment) {
    const auto *entry = bend(index);
    const auto parameters = core::toolParameters(assignment);
    if (!document_ || active_ || !entry || !included(entry->review) || !parameters) {
        return false;
    }
    if (entry->catalogTools != assignment || entry->tools != parameters) {
        auto &changed = bends_[*bendBindings_.byRecognizedBend[index]];
        changed.tools = parameters;
        changed.catalogTools = std::move(assignment);
        modified_ = true;
        ++editRevision_;
    }
    return true;
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
