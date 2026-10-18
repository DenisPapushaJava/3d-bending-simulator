#include "bending/ui/ImportController.h"

#include <QThread>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bending::ui {
using namespace application;

class ImportController::Impl {
  public:
    Impl(ImportController *controller, std::shared_ptr<const DocumentImporter> service,
         Presenter display)
        : owner(controller), importer(std::move(service)), presenter(std::move(display)) {
        if (!importer || !presenter) {
            throw std::invalid_argument("ImportController needs an importer and presenter");
        }
        worker = new QObject;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.setObjectName(QStringLiteral("cadImportWorker"));
        thread.start();
    }

    void dispatch() {
        if (stopping || running || !pending) {
            return;
        }
        const auto request = *pending;
        pending.reset();
        stop = std::stop_source{};
        const auto token = stop.get_token();
        running = true;
        // Only this task executes in the worker. Session and presentation stay in the GUI thread.
        QMetaObject::invokeMethod(
            worker,
            [this, request, token, service = importer] {
                ImportResult result =
                    ImportError{ImportErrorCode::Cancelled, "Import cancelled", {}};
                if (!token.stop_requested()) {
                    try {
                        result = service->load(
                            request.source, token, [this, request](ImportPhase phase) {
                                QMetaObject::invokeMethod(
                                    owner,
                                    [this, request, phase] {
                                        if (!stopping && session.updateProgress(request, phase)) {
                                            emit owner->stateChanged();
                                        }
                                    },
                                    Qt::QueuedConnection);
                            });
                    } catch (const std::exception &error) {
                        result = ImportError{ImportErrorCode::UnexpectedFailure, error.what(), {}};
                    } catch (...) {
                        result = ImportError{
                            ImportErrorCode::UnexpectedFailure, "Unknown import exception", {}};
                    }
                }
                QMetaObject::invokeMethod(
                    owner,
                    [this, request, result = std::move(result)]() mutable {
                        running = false;
                        if (stopping) {
                            return;
                        }
                        if (session.isCurrent(request)) {
                            if (auto document = resultDocument(result)) {
                                try {
                                    const auto *project = std::get_if<OpenedProject>(&result);
                                    const auto invalid =
                                        project ? projectSettingsError(*project) : std::nullopt;
                                    if (invalid) {
                                        result = ImportError{
                                            ImportErrorCode::InvalidProject, *invalid, {}};
                                    } else if (const auto error =
                                                   presenter(document, session.revision() + 1);
                                               !error.isEmpty()) {
                                        result = ImportError{ImportErrorCode::PresentationFailed,
                                                             error.toStdString(),
                                                             {}};
                                    }
                                } catch (const std::exception &error) {
                                    result = ImportError{
                                        ImportErrorCode::PresentationFailed, error.what(), {}};
                                }
                            }
                            session.complete(request, std::move(result));
                        }
                        dispatch();
                        emit owner->stateChanged();
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    ImportController *owner;
    std::shared_ptr<const DocumentImporter> importer;
    Presenter presenter;
    DocumentSession session;
    QThread thread;
    QObject *worker = nullptr;
    std::stop_source stop;
    std::optional<ImportRequest> pending;
    bool running = false;
    bool stopping = false;
    bool saving = false;
    std::uint64_t saveId = 0;
    QString writeError;
    BendPreviewPtr preview;
    std::optional<BendPreviewError> previewError;
    std::uint64_t previewId = 0;
    bool preparingPreview = false;
    void invalidatePreview() {
        ++previewId;
        preview.reset();
        previewError.reset();
        if (preparingPreview) {
            stop.request_stop();
            preparingPreview = false;
        }
    }
};

ImportController::ImportController(std::shared_ptr<const DocumentImporter> importer,
                                   Presenter presenter, QObject *parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(this, std::move(importer), std::move(presenter))) {}

ImportController::~ImportController() {
    shutdown();
}

void ImportController::open(std::filesystem::path path) {
    if (impl_->stopping) {
        return;
    }
    impl_->stop.request_stop();
    impl_->invalidatePreview();
    ++impl_->saveId;
    impl_->writeError.clear();
    impl_->pending = impl_->session.beginImport(std::move(path));
    impl_->dispatch();
    emit stateChanged();
}

void ImportController::cancel() {
    impl_->stop.request_stop();
    impl_->invalidatePreview();
    ++impl_->saveId;
    impl_->writeError.clear();
    impl_->pending.reset();
    impl_->session.cancel();
    emit stateChanged();
}

void ImportController::closeDocument() {
    cancel();
    impl_->presenter({}, impl_->session.revision() + 1);
    impl_->session.close();
    emit stateChanged();
}

void ImportController::shutdown() {
    if (impl_->stopping) {
        return;
    }
    impl_->stopping = true;
    impl_->stop.request_stop();
    impl_->pending.reset();
    impl_->session.cancel();
    // No terminate(): a non-interruptible OCCT call must finish before its dependencies die.
    impl_->thread.quit();
    impl_->thread.wait();
}

const DocumentSession &ImportController::session() const {
    return impl_->session;
}

bool ImportController::hasWork() const {
    return impl_->running || impl_->pending.has_value();
}

ThicknessValidation ImportController::setManualThickness(double valueMm) {
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setManualThickness(valueMm);
    if (before != impl_->session.editRevision()) {
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
void ImportController::useAutomaticThickness() {
    const auto before = impl_->session.editRevision();
    impl_->session.useAutomaticThickness();
    if (before != impl_->session.editRevision()) {
        impl_->invalidatePreview();
    }
    emit stateChanged();
}
bool ImportController::setBendReview(std::size_t recognizedIndex, BendReview review) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setBendReview(recognizedIndex, review);
    if (before != impl_->session.editRevision()) {
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendTools(std::size_t recognizedIndex,
                                    std::optional<core::PressTools> tools) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const bool result = impl_->session.setBendTools(recognizedIndex, tools);
    if (before != impl_->session.editRevision()) {
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendTarget(std::size_t recognizedIndex,
                                     std::optional<core::BendTarget> target) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setBendTarget(recognizedIndex, target);
    if (before != impl_->session.editRevision()) {
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::isSaving() const {
    return impl_->saving;
}
QString ImportController::saveError() const {
    return impl_->writeError;
}
void ImportController::saveProject(std::filesystem::path path,
                                   std::shared_ptr<const ProjectWriter> writer) {
    if (impl_->stopping || hasWork() || !impl_->session.document() || !writer) {
        return;
    }
    auto *state = impl_.get();
    const auto document = state->session.document();
    const auto manual = state->session.manualThicknessMm();
    const auto bends = state->session.bends();
    const auto revision = state->session.editRevision();
    const auto id = ++state->saveId;
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->saving = true;
    state->writeError.clear();
    QMetaObject::invokeMethod(
        state->worker,
        [state, document, manual, bends, revision, id, stop, path = std::move(path),
         writer = std::move(writer)] {
            std::optional<std::string> error;
            try {
                error = writer->save(path, *document, manual, bends, stop);
            } catch (const std::exception &failure) {
                error = failure.what();
            } catch (...) {
                error = "Unknown project save exception";
            }
            QMetaObject::invokeMethod(
                state->owner,
                [state, revision, id, path, error] {
                    state->running = false;
                    state->saving = false;
                    if (state->stopping) {
                        return;
                    }
                    if (id == state->saveId) {
                        if (error) {
                            state->writeError = QString::fromStdString(*error);
                        } else {
                            state->session.markSaved(revision, path);
                        }
                    }
                    state->dispatch();
                    emit state->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
}
bool ImportController::prepareBendPreview(std::size_t recognizedIndex, bool fixedSecond,
                                          std::shared_ptr<const SingleBendPreviewBuilder> builder,
                                          bool toolSetup) {
    if (impl_->stopping || hasWork() || !builder || !impl_->session.document()) {
        return false;
    }
    const auto *entry = impl_->session.bend(recognizedIndex);
    if (!entry || entry->review != BendReview::Confirmed || !entry->target ||
        (toolSetup && !entry->tools)) {
        return false;
    }
    const auto &metadata = impl_->session.document()->metadata();
    if (recognizedIndex >= metadata.bendAnalysis.bends.size() ||
        (impl_->session.manualThicknessMm() &&
         std::abs(*impl_->session.manualThicknessMm() -
                  metadata.bendAnalysis.bends[recognizedIndex].thicknessMm) >
             metadata.bendAnalysis.lengthToleranceMm)) {
        return false;
    }
    auto *state = impl_.get();
    state->invalidatePreview();
    const auto id = state->previewId;
    const auto document = state->session.document();
    const auto revision = state->session.revision();
    const auto editRevision = state->session.editRevision();
    const auto target = *entry->target;
    const auto tools = toolSetup ? entry->tools : std::nullopt;
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->preparingPreview = true;
    QMetaObject::invokeMethod(
        state->worker,
        [state, document, revision, editRevision, id, target, recognizedIndex, fixedSecond, stop,
         tools, builder = std::move(builder)] {
            BendPreviewResult result =
                BendPreviewError{BendPreviewErrorCode::Cancelled, "Cancelled"};
            try {
                result =
                    tools ? builder->buildToolSetup(document, recognizedIndex, target, *tools, stop)
                          : builder->build(document, recognizedIndex, target, fixedSecond, stop);
            } catch (const std::exception &error) {
                result = BendPreviewError{BendPreviewErrorCode::Failed, error.what()};
            } catch (...) {
                result = BendPreviewError{BendPreviewErrorCode::Failed, "Unknown preview failure"};
            }
            QMetaObject::invokeMethod(
                state->owner,
                [state, revision, editRevision, id, result = std::move(result)] {
                    state->running = false;
                    state->preparingPreview = false;
                    if (state->stopping) {
                        return;
                    }
                    if (id == state->previewId && revision == state->session.revision() &&
                        editRevision == state->session.editRevision() &&
                        !state->session.activeRequest()) {
                        if (const auto *preview = std::get_if<BendPreviewPtr>(&result)) {
                            if (*preview &&
                                ((*preview)->toolSetup() ? (*preview)->frameCount() == 1
                                                         : (*preview)->frameCount() >= 2)) {
                                state->preview = *preview;
                            } else {
                                state->previewError =
                                    BendPreviewError{BendPreviewErrorCode::Failed, "Empty preview"};
                            }
                        } else if (const auto &error = std::get<BendPreviewError>(result);
                                   error.code != BendPreviewErrorCode::Cancelled) {
                            state->previewError = error;
                        }
                    }
                    state->dispatch();
                    emit state->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
void ImportController::discardBendPreview() {
    impl_->invalidatePreview();
    emit stateChanged();
}
BendPreviewPtr ImportController::bendPreview() const {
    return impl_->preview;
}
const std::optional<BendPreviewError> &ImportController::bendPreviewError() const {
    return impl_->previewError;
}
bool ImportController::isPreparingPreview() const {
    return impl_->preparingPreview;
}
} // namespace bending::ui
