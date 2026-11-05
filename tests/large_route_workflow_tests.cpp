#include "ProfileFixtures.h"
#include "RoutePanel.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    check(done(), "Large UI workflow timeout");
    QApplication::processEvents();
}
class Builder final : public application::SingleBendPreviewBuilder {
  public:
    mutable cad::SingleBendPreviewBuilder impl;
    mutable std::atomic<unsigned> searches = 0;
    mutable std::atomic<unsigned> cancelledDrafts = 0;
    mutable std::atomic<qint64> lastDraftNs = 0;
    application::BendRouteResult buildRoute(application::DocumentPtr d,
                                            application::BendRouteRequest r,
                                            std::stop_token s) const override {
        ++searches;
        return impl.buildRoute(std::move(d), std::move(r), s);
    }
    application::RouteDraftResult buildRouteDraft(application::DocumentPtr d,
                                                  application::RouteDraftRequest r,
                                                  std::stop_token s) const override {
        QElapsedTimer timer;
        timer.start();
        auto result = impl.buildRouteDraft(std::move(d), std::move(r), s);
        if (s.stop_requested())
            ++cancelledDrafts;
        lastDraftNs = timer.nsecsElapsed();
        return result;
    }
    void discardRouteCache() const override {
        impl.discardRouteCache();
    }
    application::BendPreviewResult build(application::DocumentPtr d, std::size_t i,
                                         core::BendTarget target, bool reversed,
                                         std::stop_token stop) const override {
        return impl.build(std::move(d), i, target, reversed, stop);
    }
};
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("large-route-workflow-test");
    QTemporaryDir folder;
    try {
        check(folder.isValid(), "No temporary directory");
        const auto path = folder.path() + "/source.step";
        STEPControl_Writer w;
        check(w.Transfer(fixtures::foldedProfile(32), STEPControl_AsIs) == IFSelect_RetDone &&
                  w.Write(path.toUtf8().constData()) == IFSelect_RetDone,
              "Write failed");
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<Builder>();
        const auto pair = tool_fixture::pair(16);
        ui::MainWindow window({"test", "test"}, files, files, builder, {pair.punch, pair.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(path);
        auto &c = window.importController();
        wait([&] { return !c.hasWork(); });
        check(c.session().document() && c.session().orderedBends().size() == 32,
              "Large STEP did not reach UI");
        c.setUnfoldSettings({core::SheetMaterial::MildSteel, .45, 1});
        for (auto i : c.session().orderedBends())
            check(c.setBendToolAssignment(i, pair), "Assignment failed");
        auto *panel = window.findChild<ui::RoutePanel *>();
        auto *view = window.findChild<viewer::ViewerWidget *>();
        check(panel && view, "Missing route/viewer");
        window.findChild<QToolButton *>("processStage2")->click();
        wait([&] { return !c.hasWork(); });
        check(c.routeDraft() && c.routeDraft()->request.step == 0 &&
                  c.routeDraft()->request.withTools && c.routeDraft()->checkedToolPositions == 1,
              "Large bending stage lost checked setup");
        panel->select(32);
        wait([&] { return !c.hasWork(); });
        check(c.routeDraft() && c.routeDraft()->request.step == 32 &&
                  c.routeDraft()->checkedToolPositions == 21 && builder->searches == 0,
              "Large manual step did not check tool positions independently of search");
        auto *last = panel->stepStrip()->findChild<QPushButton *>("routeStep32");
        auto *scroll = panel->stepStrip()->findChild<QScrollArea *>();
        check(last && scroll && scroll->horizontalScrollBar()->maximum() > 0,
              "Large strip did not scroll");
        wait([&] {
            return scroll->viewport()->rect().contains(
                QRect(last->mapTo(scroll->viewport(), QPoint{}), last->size()));
        });
        check(panel->findChild<QPushButton *>("prepareBendRoute")->isEnabled(),
              "Large auto search remains disabled");
        const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
        if (!output.isEmpty()) {
            auto shot = window.grab().toImage();
            if (view->isReady()) {
                view->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
                view->fitAll();
                QApplication::processEvents();
                QPainter p(&shot);
                p.drawImage(view->mapTo(&window, QPoint()), view->captureImage(true));
            }
            check(shot.save(output), "Snapshot failed");
        }
        const auto selectedId = c.routeDraft()->request.operations.back().id;
        panel->findChild<QPushButton *>("routeMoveEarlier")->click();
        wait([&] { return !c.hasWork(); });
        check(c.routeDraft() && c.routeDraft()->request.step == 31 &&
                  c.routeDraft()->request.operations[30].id == selectedId &&
                  c.routeDraft()->checkedToolPositions == 21,
              "Moving a large step lost identity or collision checks");
        panel->select(0);
        wait([&] { return !c.hasWork(); });
        check(c.routeDraft() && c.routeDraft()->request.step == 0 &&
                  !panel->findChild<QPushButton *>("routeMoveEarlier")->isEnabled() &&
                  builder->searches == 0,
              "Flat step became reorderable");
        const bool fastPrefetch = app.arguments().contains("--audit-prefetch-fast");
        const bool auditPrefetch = fastPrefetch || app.arguments().contains("--audit-prefetch");
        if (app.arguments().contains("--audit-transitions") || auditPrefetch) {
            QElapsedTimer heartbeat, total;
            QTimer pulse;
            pulse.setInterval(10);
            qint64 maxGuiGapMs = 0;
            QObject::connect(&pulse, &QTimer::timeout,
                             [&] { maxGuiGapMs = std::max(maxGuiGapMs, heartbeat.restart()); });
            for (const int step : {1, 2, 3, 2, 3}) {
                maxGuiGapMs = 0;
                heartbeat.start();
                total.start();
                pulse.start();
                panel->select(step);
                const auto selectNs = total.nsecsElapsed();
                wait([&] { return !c.hasWork(); });
                const auto totalNs = total.nsecsElapsed();
                pulse.stop();
                check(c.routeDraft() &&
                          c.routeDraft()->request.step == static_cast<std::size_t>(step),
                      "Audited step was not prepared");
                std::cout << "transition step=" << step
                          << " selectMs=" << static_cast<double>(selectNs) / 1.e6
                          << " workerMs=" << static_cast<double>(builder->lastDraftNs.load()) / 1.e6
                          << " totalMs=" << static_cast<double>(totalNs) / 1.e6
                          << " maxGuiGapMs=" << maxGuiGapMs << '\n';
                if (auditPrefetch && step < 3) {
                    const auto current = c.routeDraft();
                    const auto revision = c.session().editRevision();
                    QElapsedTimer preparation;
                    preparation.start();
                    check(c.prefetchNextRouteDraft(builder), "Cannot prefetch the next step");
                    check(!c.hasWork() && !c.isPreparingDraft(), "Speculation blocked playback");
                    wait([&] { return !c.isPrefetchingRouteDraft(); });
                    check(c.routeDraft() == current && c.session().editRevision() == revision,
                          "Speculation changed the current step or document");
                    std::cout << "prefetch next=" << step + 1
                              << " backgroundMs=" << preparation.elapsed() << '\n';
                }
            }
            if (auditPrefetch) {
                builder->impl.discardRouteCache();
                panel->select(1);
                wait([&] { return !c.hasWork(); });
                auto *slider = window.findChild<QSlider *>("bendFrame");
                auto *play = window.findChild<QPushButton *>("playBend");
                auto *timer = window.findChild<QTimer *>("bendPlaybackTimer");
                check(slider && play && timer, "Playback controls missing");
                auto *speed = window.findChild<QSlider *>("bendPlaybackSpeed");
                speed->setValue(fastPrefetch ? speed->maximum() : 0);
                slider->setValue(0);
                const auto first = c.routeDraft();
                const auto cancellations = builder->cancelledDrafts.load();
                check(first && play->isEnabled(), "Playback fixture is blocked at its first frame");
                play->click();
                check(timer->isActive() && c.isPrefetchingRouteDraft() && !c.hasWork(),
                      "Playback did not start background preparation");
                wait([&] { return slider->value() >= 2 || !timer->isActive(); });
                check(timer->isActive() && c.routeDraft() == first && slider->value() >= 2 &&
                          !c.hasWork(),
                      "Speculation paused/replaced the current animation");
                wait([&] {
                    return c.routeDraft() && c.routeDraft()->request.step == 3 && timer->isActive();
                });
                check(builder->cancelledDrafts == cancellations,
                      "Auto-advance cancelled and restarted a matching speculative job");
                play->click();
                check(!timer->isActive(), "Pause did not stop playback");
                wait([&] { return !c.isPrefetchingRouteDraft(); });
                check(c.routeDraft()->request.step == 3 && builder->searches == 0,
                      "Pause applied speculative geometry or started a route search");
                std::cout << "playback advanced 1 -> 2 -> 3 with read-ahead and paused safely\n";
            }
        }
        c.shutdown();
        std::cout << "32-step strip, scroll, identity and checked tool frames passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
