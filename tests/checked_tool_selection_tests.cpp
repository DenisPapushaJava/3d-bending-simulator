#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace bending;
using namespace bending::application;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &ready) {
    QElapsedTimer t;
    t.start();
    while (!ready() && t.elapsed() < 180000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    check(ready(), "Checked-tool workflow timeout");
    QApplication::processEvents();
}
class Builder final : public SingleBendPreviewBuilder {
  public:
    enum class Mode { Real, WrongIndex, Wait };
    mutable cad::SingleBendPreviewBuilder impl;
    mutable BendRouteRequest last;
    mutable BendRoutePtr accepted;
    mutable std::atomic<unsigned> calls = 0;
    std::atomic<Mode> mode = Mode::Real;
    mutable std::atomic<bool> entered = false, release = false, stopObserved = false;
    BendPreviewResult build(DocumentPtr d, std::size_t i, core::BendTarget t, bool reverse,
                            std::stop_token stop) const override {
        return impl.build(std::move(d), i, t, reverse, stop);
    }
    RouteDraftResult buildRouteDraft(DocumentPtr d, RouteDraftRequest r,
                                     std::stop_token stop) const override {
        return impl.buildRouteDraft(std::move(d), std::move(r), stop);
    }
    void discardRouteCache() const override {
        impl.discardRouteCache();
    }
    BendRouteResult buildRoute(DocumentPtr d, BendRouteRequest r,
                               std::stop_token stop) const override {
        last = r;
        ++calls;
        if (mode == Mode::WrongIndex) {
            auto broken = std::make_shared<BendRoute>(*accepted);
            broken->selectedToolVariant = r.toolVariants.size();
            return BendRoutePtr(broken);
        }
        if (mode == Mode::Wait) {
            if (r.onProgress)
                r.onProgress({BendRoutePhase::Checking, 0, r.toolVariants.size(), 5, 0});
            entered = true;
            QElapsedTimer timer;
            timer.start();
            while (!release && timer.elapsed() < 10000) {
                stopObserved = stop.stop_requested();
                QThread::msleep(2);
            }
            if (r.onProgress)
                r.onProgress({BendRoutePhase::Checking, 0, r.toolVariants.size(), 99, 0});
            // Deliberately return an earlier successful result after cancellation.
            return accepted;
        }
        auto result = impl.buildRoute(std::move(d), std::move(r), stop);
        if (const auto *p = std::get_if<BendRoutePtr>(&result))
            accepted = *p;
        return result;
    }
};
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temp;
    try {
        check(temp.isValid(), "No temporary directory");
        ui::setPreferencesFile(temp.path() + "/settings.ini");
        auto pass = tool_fixture::pair(16);
        pass.punch.id = "b-clear";
        pass.punch.code = "Clear";
        pass.punch.segments = std::vector<core::ToolSegment>{{40, 3}};
        pass.die.segments = std::vector<core::ToolSegment>{{40, 3}};
        auto blocked = pass.punch;
        blocked.id = "a-blocked";
        blocked.code = "Blocked";
        auto &edges = blocked.drawing->edges;
        edges[2].end.x = edges[3].start.x = 80;
        edges[3].end.x = edges[4].start.x = -80;
        check(core::hasCompleteDrawingToolGeometry({blocked, pass.die, 0, 3, "", true}),
              "Blocked fixture lost a supported working profile");
        const auto path = temp.path() + "/part.step";
        STEPControl_Writer writer;
        check(writer.Transfer(fixtures::channel(80, 60, 40), STEPControl_AsIs) ==
                      IFSelect_RetDone &&
                  writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
              "Write fixture failed");
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<Builder>();
        ui::MainWindow window({"test", "test"}, files, files, builder,
                              {blocked, pass.punch, pass.die});
        struct ReleaseOnExit {
            Builder &builder;
            ~ReleaseOnExit() {
                builder.release = true;
            }
        } release{*builder};
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(path);
        auto &c = window.importController();
        wait([&] { return !c.hasWork(); });
        const auto document = c.session().document();
        check(document && document->metadata().bendAnalysis.bends.size() == 2,
              "Import lost the two bends");
        const auto source = std::string(document->sourceStep());
        const auto beforeSuggestion = c.session().editRevision();
        window.findChild<QToolButton *>("processStage1")->click();
        wait([&] { return !c.hasWork(); });
        check(builder->calls == 0 && !c.session().bend(0)->catalogTools &&
                  c.session().editRevision() == beforeSuggestion &&
                  window.findChild<QLabel *>("toolRecommendation")
                      ->text()
                      .contains(QStringLiteral("Предварительный")),
              "Initial tool proposal ran a route or committed an assignment");
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        auto *recommend = window.findChild<QPushButton *>("recommendTools");
        check(recommend && recommend->isEnabled(), "No checked selection button");
        recommend->click();
        wait([&] { return !c.hasWork(); });
        if (const auto e = c.bendRouteError())
            throw std::runtime_error("Checked selection: " + e->detail);
        const auto route = c.bendRoute();
        check(route && route->steps.size() == 2 && route->selectedToolVariant &&
                  *route->selectedToolVariant >= 2 && route->statistics.testedToolVariants >= 3,
              "Blocked profiles were not replaced by a checked candidate");
        for (const auto &step : route->steps) {
            const auto *entry = c.session().bend(step.bendIndex);
            check(entry->catalogTools && entry->catalogTools->punch.id == pass.punch.id &&
                      entry->catalogTools->die.id == pass.die.id &&
                      entry->catalogTools->punchAssembly && entry->catalogTools->dieAssembly &&
                      step.preview->frameCount() == 61 && step.preview->punchRetraction() &&
                      !step.preview->collision(),
                  "Accepted assignment, sections or checked cycle differ");
        }
        auto *punch = window.findChild<QComboBox *>("punchPreset");
        auto *message = window.findChild<QLabel *>("toolRecommendation");
        check(punch->currentText().contains("Clear") &&
                  message->text().contains(QStringLiteral("Проверенный")),
              "Winning tool was not shown in the tool stage");
        const auto snapshot = qEnvironmentVariable("BENDING_TOOL_SELECTION_SNAPSHOT");
        if (!snapshot.isEmpty()) {
            auto shot = window.grab().toImage();
            auto *view = window.findChild<viewer::ViewerWidget *>();
            if (view && view->isVisible() && view->isReady()) {
                QPainter painter(&shot);
                painter.drawImage(view->mapTo(&window, QPoint()), view->captureImage(true));
            }
            check(shot.save(snapshot), "Snapshot failed");
        }
        auto limited = builder->last;
        limited.maximumChecks = 1;
        const auto limit = builder->impl.buildRoute(document, limited, {});
        check(std::get<BendRouteError>(limit).code == BendRouteErrorCode::SearchLimit,
              "Incomplete tooling search became a rejected tool");
        auto narrow = builder->last;
        narrow.toolVariants = {{tool_fixture::pair(4), {}}};
        check(std::get<BendRouteError>(builder->impl.buildRoute(document, narrow, {})).code ==
                  BendRouteErrorCode::InvalidInput,
              "Checked selection admitted an undersized opening");
        const auto saved = c.session().bend(0)->catalogTools;
        check(window.findChild<QToolButton *>("processStage2")
                  ->accessibleName()
                  .contains(QStringLiteral("Порядок")),
              "The stage summary still requests a check after successful selection");
        window.findChild<QToolButton *>("processStage2")->click();
        wait([&] { return !c.hasWork(); });
        check(c.bendRoute() == route, "Entering bending discarded the checked route");
        window.findChild<QToolButton *>("processStage1")->click();
        wait([&] { return !c.hasWork(); });
        builder->mode = Builder::Mode::WrongIndex;
        recommend->click();
        wait([&] { return !c.hasWork(); });
        check(!c.bendRoute() && c.bendRouteError() &&
                  c.bendRouteError()->code == BendRouteErrorCode::Failed &&
                  c.session().bend(0)->catalogTools == saved,
              "Invalid selected index changed the document");
        builder->mode = Builder::Mode::Wait;
        recommend->click();
        wait([&] { return builder->entered.load(); });
        auto *progress = window.findChild<QDialog *>("checkedToolProgress");
        auto *indicator = window.findChild<QProgressBar *>("checkedToolBusy");
        check(progress && progress->isVisible() && !progress->isModal() && window.isEnabled() &&
                  indicator && indicator->minimum() == 0 && indicator->maximum() == 0 &&
                  c.bendRouteProgress() && c.bendRouteProgress()->phase == BendRoutePhase::Checking,
              "Live progress blocked the app or invented a percentage");
        if (!snapshot.isEmpty())
            check(progress->grab().save(snapshot + ".progress.png"), "Progress snapshot failed");
        auto *quick = window.findChild<QAction *>("recommendToolsQuick");
        auto *options = window.findChild<QToolButton *>("recommendToolsOptions");
        check(quick && quick->isEnabled() && options->isEnabled() && punch->isEnabled(),
              "Background selection disabled quick/manual alternatives");
        quick->trigger();
        wait([&] { return builder->stopObserved.load(); });
        check(c.hasWork() && !progress->isVisible() && punch->currentText().contains("Blocked") &&
                  c.session().bend(0)->catalogTools == saved,
              "Quick fallback waited for CAD or changed confirmed assignments");
        builder->release = true;
        wait([&] { return !c.hasWork(); });
        check(!c.bendRoute() && punch->currentText().contains("Blocked") &&
                  c.session().bend(0)->catalogTools == saved,
              "Late result overwrote quick/manual selection");
        const auto profile = punch->currentData();
        window.findChild<QPushButton *>("fitSelectedToolSections")->click();
        check(punch->currentData() == profile && c.session().bend(0)->catalogTools == saved,
              "Section selection replaced the profile or committed a draft");
        window.findChild<QToolButton *>("processStage0")->click();
        window.findChild<QToolButton *>("processStage1")->click();
        wait([&] { return !c.hasWork(); });
        check(punch->currentData() == profile, "Re-entering tools overwrote a manual draft");
        builder->entered = builder->release = builder->stopObserved = false;
        recommend->click();
        wait([&] { return builder->entered.load(); });
        auto *cancel = window.findChild<QAction *>("cancelImportAction");
        check(cancel && cancel->isEnabled(), "Global cancellation is unavailable");
        cancel->trigger();
        wait([&] { return builder->stopObserved.load(); });
        check(!progress->isVisible() && c.hasWork(),
              "Global cancellation left progress blocking the app");
        builder->release = true;
        wait([&] { return !c.hasWork(); });
        check(!c.bendRoute() && !c.bendRouteProgress() &&
                  c.session().bend(0)->catalogTools == saved,
              "Late progress/result survived global cancellation");
        check(document->sourceStep() == source, "Selection changed original CAD bytes");
        c.shutdown();
        std::cout << "Checked alternate profiles, sections, guards, cancellation and quick "
                     "fallback passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
