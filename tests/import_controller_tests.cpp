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
} // namespace
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testController();
        testShutdown();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Import controller: responsive GUI, latest-only queue, cancel, close, failure and "
                 "shutdown passed\n";
}
