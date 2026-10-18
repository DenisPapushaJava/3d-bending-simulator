#include "bending/ui/ImportController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
using namespace bending::application;
using bending::ui::ImportController;
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void waitUntil(const std::function<bool()> &condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), "Controller wait timed out");
}
class TestDocument final : public Document {
  public:
    explicit TestDocument(std::filesystem::path source) {
        data_.sourcePath = std::move(source);
        data_.bendAnalysis.status = BendRecognitionStatus::Completed;
        data_.bendAnalysis.bends = {
            {{0, 0, 0}, {0, 0, 80}, 1.5, 4, 2, 80, 1, 2, {3, 4, 5, 6}, false, 80}};
    }
    const DocumentMetadata &metadata() const noexcept override {
        return data_;
    }
    const std::vector<FaceInfo> &faces() const noexcept override {
        return faces_;
    }

  private:
    DocumentMetadata data_{};
    std::vector<FaceInfo> faces_;
};
class GatedImporter final : public DocumentImporter {
  public:
    ImportResult load(const std::filesystem::path &path, std::stop_token stop,
                      const ImportProgress &progress) const override {
        require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                "Importer ran on GUI thread");
        progress(ImportPhase::Reading);
        if (path == "slow.step" || path == "cooperative.step") {
            std::unique_lock lock(mutex);
            entered = true;
            std::stop_callback wake(stop, [this] { condition.notify_all(); });
            condition.wait(lock, [&] {
                return released || (path == "cooperative.step" && stop.stop_requested());
            });
            if (path == "cooperative.step" && stop.stop_requested()) {
                return ImportError{ImportErrorCode::Cancelled, "stop", {}};
            }
            // Deliberately ignore stop to emulate an uninterruptible SDK call and a late success.
        }
        if (path == "middle.step") {
            middleCalls++;
        }
        if (path == "broken.step") {
            return ImportError{ImportErrorCode::InvalidStep, "broken", {}};
        }
        progress(ImportPhase::Preparing);
        if (path == "invalid-project.bend") {
            return OpenedProject{std::make_shared<TestDocument>(path),
                                 {},
                                 path,
                                 {{"invalid", {}, BendReview::Confirmed, {}}}};
        }
        return DocumentPtr(std::make_shared<TestDocument>(path));
    }
    void reset() {
        std::lock_guard lock(mutex);
        released = false;
        entered = false;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable bool released = false;
    mutable std::atomic<bool> entered{false};
    mutable std::atomic<int> middleCalls{0};
};
// Releases a deliberately blocked test adapter before controller destruction even on assertion
// failure.
struct ReleaseGate {
    std::shared_ptr<GatedImporter> importer;
    ~ReleaseGate() {
        importer->release();
    }
};
void testController() {
    auto importer = std::make_shared<GatedImporter>();
    std::vector<std::filesystem::path> presented;
    ImportController controller(importer, [&](DocumentPtr document, std::uint64_t) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                "Presenter ran outside GUI thread");
        if (document && document->metadata().sourcePath == "cannot-display.step") {
            return QStringLiteral("display failure");
        }
        if (document) {
            presented.push_back(document->metadata().sourcePath);
        }
        return QString{};
    });
    ReleaseGate release{importer};
    QObject::connect(&controller, &ImportController::stateChanged, [&] {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                "State emitted outside GUI thread");
    });
    const auto finished = [&] {
        return !controller.session().activeRequest() && !controller.hasWork();
    };
    controller.open("seed.step");
    waitUntil(finished);
    const auto seed = controller.session().document();
    controller.open("slow.step");
    waitUntil([&] { return importer->entered.load(); });
    int beats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start(5);
    waitUntil([&] { return beats >= 5; });
    require(controller.session().document() == seed, "Pending work changed current document");
    controller.open("middle.step");
    controller.open("latest.step");
    importer->release();
    waitUntil(finished);
    require(controller.session().document()->metadata().sourcePath == "latest.step",
            "Latest request was not applied");
    require(controller.session().revision() == 2 && importer->middleCalls == 0,
            "Pending requests were not coalesced");
    require(presented == std::vector<std::filesystem::path>{"seed.step", "latest.step"},
            "Stale geometry reached presenter");
    const auto latest = controller.session().document();
    controller.open("broken.step");
    waitUntil(finished);
    require(controller.session().error() && controller.session().document() == latest,
            "Import error replaced current document");
    controller.open("cannot-display.step");
    waitUntil(finished);
    require(controller.session().error()->code == ImportErrorCode::PresentationFailed &&
                controller.session().revision() == 2,
            "Presentation failure was committed");
    const auto beforeInvalid = presented.size();
    controller.open("invalid-project.bend");
    waitUntil(finished);
    require(controller.session().error()->code == ImportErrorCode::InvalidProject &&
                controller.session().document() == latest && presented.size() == beforeInvalid,
            "Invalid saved preparations reached presenter or replaced scene");
    importer->reset();
    controller.open("slow.step");
    waitUntil([&] { return importer->entered.load(); });
    controller.cancel();
    importer->release();
    waitUntil(finished);
    require(controller.session().document() == latest && controller.session().revision() == 2,
            "Late cancelled success was committed");
    importer->reset();
    controller.open("slow.step");
    waitUntil([&] { return importer->entered.load(); });
    controller.closeDocument();
    importer->release();
    waitUntil(finished);
    require(!controller.session().document() && controller.session().revision() == 3,
            "Late reply reopened closed document");
}
void testShutdown() {
    auto importer = std::make_shared<GatedImporter>();
    auto controller = std::make_unique<ImportController>(
        importer, [](DocumentPtr, std::uint64_t) { return QString{}; });
    controller->open("cooperative.step");
    waitUntil([&] { return importer->entered.load(); });
    QElapsedTimer elapsed;
    elapsed.start();
    controller.reset();
    require(elapsed.elapsed() < 2000, "Cooperative shutdown hung");
    QCoreApplication::processEvents();
}
class GatedWriter final : public ProjectWriter {
  public:
    std::optional<std::string> save(const std::filesystem::path &path, const Document &,
                                    std::optional<double>,
                                    const std::vector<BendPreparation> &bends,
                                    std::stop_token stop) const override {
        require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                "Save ran in GUI thread");
        if (path == "failed.bend") {
            return "disk error";
        }
        captured = bends;
        entered = true;
        while (!stop.stop_requested()) {
            QThread::msleep(1);
        }
        return {}; // Emulate a completed atomic rename racing with cancellation.
    }
    mutable std::atomic<bool> entered{false};
    mutable std::vector<BendPreparation> captured;
};
void testSaveRace() {
    const auto importer = std::make_shared<GatedImporter>();
    const auto writer = std::make_shared<GatedWriter>();
    ImportController controller(importer, [](DocumentPtr, std::uint64_t) { return QString{}; });
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    controller.open("seed.step");
    waitUntil(finished);
    require(controller.setManualThickness(2.0) == ThicknessValidation::Unverified,
            "Manual edit failed");
    require(controller.setBendReview(0, BendReview::Confirmed) &&
                controller.setBendTarget(0, bending::core::BendTarget{2, 5}),
            "Bend preparation failed");
    controller.saveProject("failed.bend", writer);
    waitUntil(finished);
    require(!controller.saveError().isEmpty() && controller.session().modified(),
            "Failed write cleared unsaved edits");
    controller.saveProject("old.bend", writer);
    waitUntil([&] { return writer->entered.load(); });
    require(writer->captured.size() == 1 && writer->captured[0].review == BendReview::Confirmed &&
                writer->captured[0].target == bending::core::BendTarget{2, 5} &&
                !controller.setBendReview(0, BendReview::Excluded),
            "Writer did not receive immutable preparation snapshot or allowed edits during save");
    int beats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start(1);
    waitUntil([&] { return beats >= 5; });
    controller.open("new.step");
    waitUntil(finished);
    require(controller.session().document()->metadata().sourcePath == "new.step" &&
                controller.session().projectPath().empty() &&
                !controller.session().manualThicknessMm() &&
                controller.session().bend(0)->review == BendReview::Pending &&
                !controller.session().bend(0)->target,
            "Late save rebound a new document to an old project");
    writer->entered = false;
    controller.saveProject("closing.bend", writer);
    waitUntil([&] { return writer->entered.load(); });
    controller.closeDocument();
    waitUntil(finished);
    require(!controller.session().document() && controller.session().projectPath().empty(),
            "Late save resurrected a closed project");
}
class TestPreview final : public SingleBendPreview {
  public:
    explicit TestPreview(bool tools = false) : withTools_(tools) {}
    const bending::simulation::FlatToolSetup *toolSetup() const noexcept override {
        return withTools_ ? &setup_ : nullptr;
    }
    const bending::simulation::SingleBend &model() const noexcept override {
        return model_;
    }
    std::size_t frameCount() const noexcept override {
        return withTools_ ? 1 : 21;
    }
    BoundsMm bounds() const noexcept override {
        return {{-30, -2, 0}, {50, 40, 80}};
    }

  private:
    bool withTools_;
    bending::simulation::FlatToolSetup setup_{};
    bending::simulation::SingleBend model_{2, 4, 1.5, 80, 30, 30};
};
class GatedPreviewBuilder final : public SingleBendPreviewBuilder {
  public:
    BendPreviewResult buildToolSetup(DocumentPtr document, std::size_t index,
                                     bending::core::BendTarget target, bending::core::PressTools,
                                     std::stop_token stop) const override {
        const auto result = build(document, index, target, false, stop);
        if (const auto *failure = std::get_if<BendPreviewError>(&result)) {
            return *failure;
        }
        return BendPreviewPtr(std::make_shared<TestPreview>(true));
    }
    BendPreviewResult build(DocumentPtr, std::size_t, bending::core::BendTarget, bool,
                            std::stop_token) const override {
        require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                "Preview built in GUI thread");
        std::unique_lock lock(mutex);
        entered = true;
        condition.wait(lock, [&] { return released; }); // Deliberately ignore cancellation.
        if (fail) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedGeometry, "unsupported"};
        }
        return BendPreviewPtr(std::make_shared<TestPreview>());
    }
    void reset() {
        std::lock_guard lock(mutex);
        entered = false;
        released = false;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable std::atomic<bool> entered{false};
    mutable bool released = true;
    std::atomic<bool> fail{false};
};
void testPreviewLifecycle(bool toolsMode = false) {
    const auto importer = std::make_shared<GatedImporter>();
    const auto builder = std::make_shared<GatedPreviewBuilder>();
    ImportController controller(importer, [](DocumentPtr, std::uint64_t) { return QString{}; });
    struct ReleasePreview {
        std::shared_ptr<GatedPreviewBuilder> service;
        ~ReleasePreview() {
            service->release();
        }
    } release{builder};
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    const auto prepare = [&] {
        if (toolsMode) {
            require(
                controller.setBendReview(0, BendReview::Confirmed) &&
                    controller.setBendTools(0, bending::core::PressTools{48, 1.5, 1.4, 2, 100, 10}),
                "Cannot set tools");
        }
        require(controller.setBendReview(0, BendReview::Confirmed) &&
                    controller.setBendTarget(0, bending::core::BendTarget{1.5, 4}) &&
                    controller.prepareBendPreview(0, false, builder, toolsMode),
                "Cannot start preview");
    };
    controller.open("part.step");
    waitUntil(finished);
    require(!controller.prepareBendPreview(0, false, builder, toolsMode),
            "Unconfirmed bend was simulated");
    const auto source = controller.session().document();
    prepare();
    waitUntil(finished);
    require(controller.bendPreview() && !controller.bendPreviewError() &&
                controller.session().document() == source,
            "Preview replaced source document");
    const auto revision = controller.session().editRevision();
    controller.discardBendPreview();
    require(!controller.bendPreview() && controller.session().editRevision() == revision,
            "Discarding view dirtied project");
    controller.prepareBendPreview(0, true, builder, toolsMode);
    waitUntil(finished);
    controller.setBendTarget(0, bending::core::BendTarget{2, 4});
    require(!controller.bendPreview(), "Changed target retained stale states");
    builder->reset();
    prepare();
    waitUntil([&] { return builder->entered.load(); });
    require(controller.isPreparingPreview() && !controller.setBendReview(0, BendReview::Excluded),
            "Editing accepted during preview preparation");
    int beats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start(1);
    waitUntil([&] { return beats >= 5; });
    controller.open("replacement.step");
    builder->release();
    waitUntil(finished);
    require(!controller.bendPreview() &&
                controller.session().document()->metadata().sourcePath == "replacement.step",
            "Late preview applied to a new document");
    builder->reset();
    prepare();
    waitUntil([&] { return builder->entered.load(); });
    controller.cancel();
    builder->release();
    waitUntil(finished);
    require(!controller.bendPreview() && !controller.bendPreviewError() &&
                controller.session().document(),
            "Cancelled preview was published or dropped source");
    builder->reset();
    prepare();
    waitUntil([&] { return builder->entered.load(); });
    controller.setManualThickness(3); // Direct API edit exercises revision protection.
    builder->release();
    waitUntil(finished);
    require(!controller.bendPreview(), "Preview ignored edit revision");
    require(!controller.prepareBendPreview(0, false, builder, toolsMode),
            "Conflicting manual thickness simulated");
    controller.useAutomaticThickness();
    builder->fail = true;
    prepare();
    waitUntil(finished);
    require(controller.bendPreviewError() && controller.session().document(),
            "Unsupported preview destroyed source");
    builder->fail = false;
    builder->reset();
    prepare();
    waitUntil([&] { return builder->entered.load(); });
    controller.closeDocument();
    builder->release();
    waitUntil(finished);
    require(!controller.bendPreview() && !controller.session().document(),
            "Late preview resurrected closed model");
}

} // namespace
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testController();
        testShutdown();
        testSaveRace();
        testPreviewLifecycle();
        testPreviewLifecycle(true);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Import controller: responsive GUI, latest-only queue, cancel, close, failure and "
                 "shutdown passed\n";
}
