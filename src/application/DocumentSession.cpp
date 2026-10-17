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
    if (auto *document = std::get_if<DocumentPtr>(&result); document && *document) {
        document_ = std::move(*document);
        ++revision_;
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
}
} // namespace bending::application
