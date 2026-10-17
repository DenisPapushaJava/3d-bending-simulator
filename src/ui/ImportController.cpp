#include "bending/ui/ImportController.h"

#include <QThread>

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
                                    const auto validation =
                                        project && project->manualThicknessMm
                                            ? validateThickness(*project->manualThicknessMm,
                                                                document->metadata().thickness)
                                            : ThicknessValidation::Unverified;
                                    const auto error =
                                        validation == ThicknessValidation::Invalid ||
                                                validation ==
                                                    ThicknessValidation::ConflictsWithGeometry
                                            ? QStringLiteral("Invalid saved thickness")
                                            : presenter(document, session.revision() + 1);
                                    if (!error.isEmpty()) {
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
    ++impl_->saveId;
    impl_->writeError.clear();
    impl_->pending = impl_->session.beginImport(std::move(path));
    impl_->dispatch();
    emit stateChanged();
}

void ImportController::cancel() {
    impl_->stop.request_stop();
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
    const auto result = impl_->session.setManualThickness(valueMm);
    emit stateChanged();
    return result;
}
void ImportController::useAutomaticThickness() {
    impl_->session.useAutomaticThickness();
    emit stateChanged();
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
    const auto revision = state->session.editRevision();
    const auto id = ++state->saveId;
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->saving = true;
    state->writeError.clear();
    QMetaObject::invokeMethod(
        state->worker,
        [state, document, manual, revision, id, stop, path = std::move(path),
         writer = std::move(writer)] {
            std::optional<std::string> error;
            try {
                error = writer->save(path, *document, manual, stop);
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
} // namespace bending::ui
