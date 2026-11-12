#include "DrawingLayoutController.h"
#include "DrawingSheetInternal.h"

namespace bending::ui {
DrawingLayoutController::DrawingLayoutController(QObject *parent, Composer composer)
    : QObject(parent), composer_(composer ? std::move(composer) : Composer{DrawingSheet::compose}),
      worker_(new QObject) {
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("drawingLayoutWorker"));
    thread_.start();
}
DrawingLayoutController::~DrawingLayoutController() {
    stopping_ = true;
    cancel();
    thread_.quit();
    thread_.wait(); // No detach: callbacks cannot outlive their controller.
}
void DrawingLayoutController::cancel() {
    ++generation_;
    pending_.reset();
    activeStop_.request_stop();
}
void DrawingLayoutController::request(application::BendingDrawingPtr source,
                                      std::vector<DrawingViewChoice> choices, DrawingFlatSide side,
                                      DrawingLayoutLimits limits, DrawingPaperFormat format) {
    drawing::initializeFont(); // Resource registration stays on the GUI thread.
    cancel();
    pending_ =
        Request{std::move(source), std::move(choices), side, limits, format, {}, generation_};
    dispatch();
}
void DrawingLayoutController::dispatch() {
    if (stopping_ || running_ || !pending_)
        return;
    auto job = std::move(*pending_);
    pending_.reset();
    activeStop_ = job.stop;
    running_ = true;
    QMetaObject::invokeMethod(
        worker_,
        [this, job = std::move(job)] {
            std::shared_ptr<const DrawingSheetPlan> plan;
            QString error;
            const auto token = job.stop.get_token();
            try {
                if (!token.stop_requested()) {
                    plan = composer_(job.source, job.choices, job.side, token, job.limits,
                                     job.paperFormat);
                    if (!plan || plan->source != job.source || plan->paperFormat != job.paperFormat)
                        throw std::runtime_error("Устаревшая компоновка чертежа");
                }
            } catch (const DrawingLayoutInterrupted &e) {
                if (e.reason != DrawingLayoutInterruption::Cancelled)
                    error = QString::fromUtf8(e.what());
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
            } catch (...) {
                error = QStringLiteral("Не удалось выполнить компоновку");
            }
            QMetaObject::invokeMethod(
                this,
                [this, generation = job.generation, token, plan = std::move(plan),
                 error = std::move(error)] {
                    running_ = false;
                    if (stopping_)
                        return;
                    if (generation == generation_ && !token.stop_requested()) {
                        if (!error.isEmpty())
                            emit failed(error);
                        else if (plan)
                            emit ready(plan);
                    }
                    dispatch();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
} // namespace bending::ui
