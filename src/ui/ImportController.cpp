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
                            if (auto *document = std::get_if<DocumentPtr>(&result);
                                document && *document) {
                                try {
                                    const auto error = presenter(*document, session.revision() + 1);
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
    impl_->pending = impl_->session.beginImport(std::move(path));
    impl_->dispatch();
    emit stateChanged();
}

void ImportController::cancel() {
    impl_->stop.request_stop();
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
} // namespace bending::ui
