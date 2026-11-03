#include "bending/ui/ImportController.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace bending;
using namespace bending::application;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(done(), "Prefetch workflow timed out");
}
class Source final : public Document {
  public:
    explicit Source(std::filesystem::path path) {
        metadata_.sourcePath = std::move(path);
        metadata_.bendAnalysis.status = BendRecognitionStatus::Completed;
        for (int i = 0; i < 3; ++i)
            metadata_.bendAnalysis.bends.push_back({{double(i * 100), 0, 0},
                                                    {0, 0, 80},
                                                    1.5,
                                                    4,
                                                    2,
                                                    80,
                                                    1,
                                                    2,
                                                    {3, 4, 5, 6},
                                                    false,
                                                    80});
    }
    const DocumentMetadata &metadata() const noexcept override {
        return metadata_;
    }
    const std::vector<FaceInfo> &faces() const noexcept override {
        return faces_;
    }

  private:
    DocumentMetadata metadata_;
    std::vector<FaceInfo> faces_;
};
class Importer final : public DocumentImporter {
  public:
    ImportResult load(const std::filesystem::path &path, std::stop_token,
                      const ImportProgress &) const override {
        return DocumentPtr(std::make_shared<Source>(path));
    }
};
class Visual final : public SingleBendPreview {
  public:
    Visual(std::size_t frames, bool reversed) : frames_(frames), reversed_(reversed) {}
    bool unverifiedDraft() const noexcept override {
        return true;
    }
    bool workpieceReversed() const noexcept override {
        return reversed_;
    }
    const simulation::SingleBend &model() const noexcept override {
        return model_;
    }
    std::size_t frameCount() const noexcept override {
        return frames_;
    }
    BoundsMm bounds() const noexcept override {
        return {{0, 0, 0}, {80, 80, 2}};
    }

  private:
    simulation::SingleBend model_{2, 4, 1.5, 80, 40, 30};
    std::size_t frames_;
    bool reversed_;
};
class Builder final : public SingleBendPreviewBuilder {
  public:
    mutable std::atomic<unsigned> calls = 0, heavy = 0;
    mutable std::atomic<int> lastStep = -1, holdStep = -1;
    mutable std::atomic<bool> entered = false, cancelled = false, corrupt = false;
    mutable std::atomic<bool> checkStopOnDiscard = false;
    void gate(bool ignoreStop = false) {
        std::lock_guard lock(mutex_);
        entered = cancelled = false;
        block_ = true;
        release_ = false;
        ignore_ = ignoreStop;
    }
    void release() {
        std::lock_guard lock(mutex_);
        release_ = true;
        condition_.notify_all();
    }
    void discardRouteCache() const override {
        if (checkStopOnDiscard.exchange(false)) {
            std::lock_guard lock(mutex_);
            require(gatedToken_.stop_requested(), "Cache cleared before stopping speculative work");
        }
        ++epoch_;
    }
    RouteDraftResult buildRouteDraft(DocumentPtr source, RouteDraftRequest request,
                                     std::stop_token token) const override {
        require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                "Draft ran in GUI thread");
        ++calls;
        lastStep = static_cast<int>(request.step);
        const auto epoch = epoch_.load();
        for (const auto &[stamp, value] : cache_)
            if (stamp == epoch && value->source == source && value->request == request)
                return value;
        ++heavy;
        {
            std::unique_lock lock(mutex_);
            if (block_ || static_cast<int>(request.step) == holdStep) {
                block_ = false;
                gatedToken_ = token;
                entered = true;
                std::stop_callback wake(token, [&] { condition_.notify_all(); });
                condition_.wait(lock,
                                [&] { return release_ || (!ignore_ && token.stop_requested()); });
                if (token.stop_requested())
                    cancelled = true;
                if (token.stop_requested() && !ignore_)
                    return BendRouteError{BendRouteErrorCode::Cancelled, "Cancelled"};
            }
        }
        auto result = std::make_shared<RouteDraft>();
        result->source = std::move(source);
        result->request = std::move(request);
        const auto &op =
            result->request.operations[result->request.step ? result->request.step - 1 : 0];
        result->visual = std::make_shared<Visual>(corrupt.exchange(false) ? 2
                                                  : result->request.step  ? 21
                                                                          : 1,
                                                  op.workpieceReversed);
        cache_.push_back({epoch, result});
        return RouteDraftPtr(result);
    }
    BendPreviewResult build(DocumentPtr, std::size_t, core::BendTarget, bool,
                            std::stop_token) const override {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedGeometry, "Unused"};
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable bool block_ = false, release_ = false, ignore_ = false;
    mutable std::atomic<unsigned> epoch_ = 0;
    mutable std::stop_token gatedToken_;
    mutable std::vector<std::pair<unsigned, RouteDraftPtr>> cache_; // Worker-only test cache.
};
struct Release {
    std::shared_ptr<Builder> builder;
    ~Release() {
        builder->release();
    }
};
RouteDraftRequest request(const ui::ImportController &controller, std::size_t step) {
    RouteDraftRequest result;
    result.step = step;
    for (auto index : controller.session().orderedBends()) {
        const auto *bend = controller.session().bend(index);
        result.operations.push_back({bend->id, index, *controller.session().bendTarget(index),
                                     bend->tools, bend->catalogTools, bend->workpieceReversed});
    }
    return result;
}
void run() {
    auto builder = std::make_shared<Builder>();
    ui::ImportController controller(std::make_shared<Importer>(),
                                    [](DocumentPtr, std::uint64_t) { return QString{}; });
    Release release{builder};
    const auto idle = [&] {
        return !controller.hasWork() && !controller.isPrefetchingRouteDraft();
    };
    controller.open("source.step");
    wait(idle);
    const auto prepare = [&](std::size_t step) {
        require(controller.prepareRouteDraft(request(controller, step), builder), "Draft refused");
        wait(idle);
        require(controller.routeDraft() && controller.routeDraft()->request.step == step,
                "Wrong foreground draft");
    };
    prepare(0);
    unsigned notifications = 0;
    QObject::connect(&controller, &ui::ImportController::stateChanged, [&] { ++notifications; });
    const auto blank = controller.routeDraft();
    builder->gate();
    require(controller.prefetchNextRouteDraft(builder), "Prefetch refused");
    wait([&] { return builder->entered.load(); });
    require(!controller.hasWork() && !controller.isPreparingDraft() &&
                controller.routeDraft() == blank && notifications == 0,
            "Speculation changed the scene or blocked foreground controls");
    const auto heavy = builder->heavy.load();
    for (int i = 0; i < 10; ++i)
        require(controller.prefetchNextRouteDraft(builder), "Duplicate prefetch refused");
    require(builder->heavy == heavy, "Duplicate prefetch created more work");
    require(controller.prepareRouteDraft(request(controller, 1), builder),
            "Matching draft refused");
    require(controller.hasWork(), "Matching request did not wait for the active worker");
    controller.cancelRouteDraftPrefetch(); // Leaving playback must not restart that explicit work.
    builder->release();
    wait(idle);
    require(controller.routeDraft() && controller.routeDraft()->request.step == 1 &&
                !builder->cancelled && builder->heavy == heavy,
            "Matching foreground request cancelled or rebuilt the warmed step");

    builder->gate(true);
    require(controller.prefetchNextRouteDraft(builder), "Second prefetch refused");
    wait([&] { return builder->entered.load(); });
    const auto index = controller.session().orderedBends()[1];
    require(controller.setBendWorkpieceOrientation(index, true), "Prefetch blocked editing");
    require(controller.setBendTools(index, core::PressTools{16, 1.5, 1., .8, 80, 3}),
            "Speculation blocked an explicit tool edit");
    builder->release();
    wait(idle);
    require(!controller.routeDraft(), "Late speculation published stale geometry after editing");
    prepare(2);
    require(controller.routeDraft()->visual->workpieceReversed(),
            "Old orientation escaped cache key");

    prepare(0);
    builder->gate();
    require(controller.prefetchNextRouteDraft(builder), "Priority prefetch refused");
    wait([&] { return builder->entered.load(); });
    builder->holdStep = 3;
    require(controller.prepareRouteDraft(request(controller, 3), builder), "Explicit step refused");
    wait([&] { return builder->lastStep == 3; });
    QCoreApplication::processEvents();
    require(controller.hasWork(), "Background completion cleared foreground busy state");
    builder->release();
    wait(idle);
    require(builder->cancelled && controller.routeDraft()->request.step == 3,
            "Explicit step did not preempt different speculation");
    builder->holdStep = -1;

    prepare(0);
    builder->discardRouteCache();
    builder->gate(true);
    const auto paused = controller.routeDraft();
    require(controller.prefetchNextRouteDraft(builder), "Pause prefetch refused");
    wait([&] { return builder->entered.load(); });
    controller.cancelRouteDraftPrefetch();
    require(!controller.hasWork() && controller.routeDraft() == paused,
            "Cancelling speculation changed the current scene");
    for (int i = 0; i < 10; ++i)
        require(controller.prefetchNextRouteDraft(builder), "Resume prefetch refused");
    const auto callsBeforeRelease = builder->calls.load();
    builder->release();
    wait(idle);
    require(controller.routeDraft() == paused && builder->calls == callsBeforeRelease + 1,
            "Resume queued more than one speculative request or published a scene");

    prepare(0);
    controller.cancelRouteDraftPrefetch();
    builder->discardRouteCache();
    builder->gate(true);
    require(controller.prefetchNextRouteDraft(builder), "Import prefetch refused");
    wait([&] { return builder->entered.load(); });
    builder->checkStopOnDiscard = true;
    controller.open("replacement.step");
    require(controller.hasWork(), "Import behind speculation was not busy");
    builder->release();
    wait(idle);
    require(controller.session().document()->metadata().sourcePath == "replacement.step" &&
                !controller.routeDraft(),
            "Speculation resurrected the previous document");

    prepare(0);
    builder->corrupt = true;
    require(controller.prefetchNextRouteDraft(builder), "Invalid-result prefetch refused");
    wait(idle);
    require(controller.routeDraft()->request.step == 0, "Background result was published");
    require(controller.prepareRouteDraft(request(controller, 1), builder),
            "Validation request refused");
    wait(idle);
    require(!controller.routeDraft() && controller.routeDraftError(),
            "Warm cache bypassed normal frame validation");

    prepare(0);
    controller.cancelRouteDraftPrefetch();
    builder->discardRouteCache();
    builder->gate(true);
    require(controller.prefetchNextRouteDraft(builder), "Close prefetch refused");
    wait([&] { return builder->entered.load(); });
    builder->checkStopOnDiscard = true;
    controller.closeDocument();
    require(!controller.session().document() && !controller.hasWork(),
            "Background work blocked closing the document");
    builder->release();
    wait(idle);
    require(!controller.session().document() && !controller.routeDraft(),
            "Late background result reopened the closed document");
    controller.open("shutdown.step");
    wait(idle);
    prepare(0);
    builder->discardRouteCache();
    builder->gate();
    require(controller.prefetchNextRouteDraft(builder), "Shutdown prefetch refused");
    wait([&] { return builder->entered.load(); });
    builder->checkStopOnDiscard = true;
    controller.shutdown();
    require(builder->cancelled, "Shutdown did not cancel speculative CAD work");
    QCoreApplication::processEvents();
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        run();
        std::cout << "Prefetch cache warming, priority, stale edits/imports, validation and "
                     "shutdown: OK\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
