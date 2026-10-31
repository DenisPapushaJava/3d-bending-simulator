#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/core/ToolGeometry.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 90000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Picker workflow timeout");
    QApplication::processEvents();
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No temporary folder");
        ui::setPreferencesFile(folder.path() + "/settings.ini");
        const auto pair = tool_fixture::pair();
        auto die = tool_fixture::fourWayDie({18, 12, 24, 30});
        die.segments = std::vector<core::ToolSegment>{{20, 3}, {40, 2}};
        auto missing = pair.die;
        missing.id = "missing-profile";
        missing.code = "Missing profile";
        missing.drawing.reset();
        auto teaching = pair.punch;
        teaching.id = "teaching";
        teaching.teaching = true;
        auto hook = tool_fixture::hook().punch;
        hook.id = "hook";
        hook.code = "Hook";
        const auto offset = tool_fixture::offsetPunch();
        const auto mirrored = tool_fixture::offsetPunch(true);
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        ui::MainWindow window({"test", "test"}, files, files, builder,
                              {pair.punch, hook, offset, mirrored, die, missing, teaching});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.raise();
        window.activateWindow();
        // Let the window manager assign the native screen before opening an anchored popup.
        wait([&] {
            return window.windowHandle() && window.windowHandle()->isExposed() &&
                   window.isActiveWindow();
        });
        auto &controller = window.importController();
        // Selection/guards and the existing checker; no production-suitability assertion.
        window.findChild<QToolButton *>("processStage1")->click();
        QApplication::processEvents();
        auto *punch = window.findChild<QToolButton *>("punchProfile");
        auto *matrix = window.findChild<QToolButton *>("dieProfile");
        require(punch && matrix && punch->isVisible() && matrix->isVisible(),
                "Clickable profiles missing");
        for (auto name : {"punchPreset", "diePreset", "dieGroove", "toolSourceMode"})
            require(!window.findChild<QComboBox *>(name)->isVisible(),
                    "Text selector remains visible");
        require(!window.findChild<QCheckBox *>("useDrawingToolGeometry")->isVisible(),
                "Conditional mode switch remains visible");
        const auto edit = controller.session().editRevision();
        unsigned calculations = 0;
        QObject::connect(&controller, &ui::ImportController::stateChanged, &window, [&] {
            calculations += controller.isPreparingPreview() || controller.isPreparingDraft() ||
                            controller.isPreparingRoute();
        });
        const auto select = [&](QToolButton *anchor, const std::string &id, std::size_t groove,
                                bool cancel = false) {
            std::string failure;
            QTimer::singleShot(50, &window, [&] {
                auto *popup = window.findChild<QDialog *>("toolPickerPopup");
                if (!popup) {
                    failure = "Picker did not open";
                    if (auto *active = QApplication::activePopupWidget())
                        active->close();
                    return;
                }
                try {
                    auto *screen =
                        QGuiApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()));
                    const auto available =
                        (screen ? screen : anchor->screen())->availableGeometry();
                    if (!available.contains(popup->geometry())) {
                        const auto r = popup->geometry(), a = available;
                        std::cerr << "Popup " << r.x() << ',' << r.y() << ' ' << r.width() << 'x'
                                  << r.height() << " available " << a.x() << ',' << a.y() << ' '
                                  << a.width() << 'x' << a.height() << '\n';
                        const auto p = popup->mapToGlobal(QPoint()),
                                   point = anchor->mapToGlobal(QPoint()),
                                   centre = anchor->mapToGlobal(anchor->rect().center());
                        std::cerr << "window=" << popup->isWindow() << " global=" << p.x() << ','
                                  << p.y() << " anchor=" << point.x() << ',' << point.y()
                                  << " centre=" << centre.x() << ',' << centre.y()
                                  << " parent=" << popup->parentWidget()->objectName().toStdString()
                                  << '\n';
                    }
                    require(available.contains(popup->geometry()),
                            "Popup exceeds the available screen");
                    require(!popup->findChild<QToolButton *>("toolOption_teaching_0"),
                            "Teaching tool leaked into palette");
                    if (auto *unavailable =
                            popup->findChild<QToolButton *>("toolOption_missing-profile_0"))
                        require(!unavailable->isEnabled(), "A missing drawing is selectable");
                    if (cancel) {
                        auto *search = popup->findChild<QLineEdit *>("toolPickerSearch");
                        search->setText("no-such-tool");
                        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                        QApplication::sendEvent(popup, &escape);
                        return;
                    }
                    auto *option =
                        popup->findChild<QToolButton *>(QStringLiteral("toolOption_%1_%2")
                                                            .arg(QString::fromStdString(id))
                                                            .arg(groove));
                    require(option && option->isEnabled(), "Working tool/variant unavailable");
                    if (id == die.id) {
                        std::vector<int> indices;
                        for (auto *button : popup->findChildren<QToolButton *>())
                            if (button->property("toolId").toString().toStdString() == die.id)
                                indices.push_back(button->property("grooveIndex").toInt());
                        require(indices == std::vector<int>({1, 0, 2, 3}),
                                "Sorting variants changed their stored groove indices");
                        const auto shot = qEnvironmentVariable("BENDING_TOOL_PICKER_SNAPSHOT");
                        if (!shot.isEmpty())
                            require(popup->grab().save(shot + "-palette.png"),
                                    "Cannot save picker image");
                    }
                    option->setFocus();
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(option, &enter);
                } catch (const std::exception &error) {
                    failure = error.what();
                    popup->reject();
                }
            });
            anchor->setFocus();
            anchor->click();
            QApplication::processEvents();
            if (!failure.empty())
                throw std::runtime_error(failure);
            require(QApplication::focusWidget() == anchor, "Focus did not return to the profile");
        };
        select(punch, pair.punch.id, 0);
        require(punch->accessibleName().contains("P60"), "Independent punch selection failed");
        // Punch-side preference must work before a die or valid assembly exists.
        select(punch, mirrored.id, 0);
        select(matrix, die.id, 3);
        auto *groove = window.findChild<QComboBox *>("dieGroove");
        require(groove->currentIndex() == 3, "Graphical variant changed the saved groove index");
        auto image = matrix->grab().toImage();
        window.findChild<QToolButton *>("activeGroove0")->click();
        require(groove->currentIndex() == 0 && matrix->grab().toImage() != image,
                "Working face did not change its actual contour");
        select(matrix, die.id, 0, true);
        require(groove->currentIndex() == 0 && !controller.hasWork() && calculations == 0 &&
                    controller.session().editRevision() == edit,
                "Cancel/selection changed project or started CAD");
        const auto file = folder.path() + "/part.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(8, 6, std::numbers::pi / 2, 40, true, 40),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture write failed");
        window.openStep(file);
        wait([&] { return !controller.hasWork(); });
        require(controller.session().document() != nullptr, "Part import failed");
        require(controller.session().document()->metadata().bendAnalysis.bends.size() == 1 &&
                    controller.session().document()->metadata().bendAnalysis.bends[0].thicknessMm ==
                        6,
                "Expected a recognized 6-mm bend");
        window.findChild<QToolButton *>("processStage1")->click();
        for (std::size_t i = 0; i < 3; ++i)
            require(!window.findChild<QToolButton *>(QStringLiteral("activeGroove%1").arg(i))
                         ->isEnabled(),
                    "Narrow V is active in the workspace");
        require(window.findChild<QToolButton *>("activeGroove3")->isEnabled(),
                "Allowed V was disabled with the whole body");
        select(matrix, die.id, 3);
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        // Applying a selection must preserve the physical stock instead of copying it per V.
        window.findChild<QPushButton *>("applyToolSetup")->click();
        wait([&] { return !controller.hasWork(); });
        if (const auto error = controller.bendPreviewError())
            throw std::runtime_error(error->detail);
        require(controller.bendPreview() && controller.bendPreview()->usesDrawingTools(),
                "Graphical choice did not reach the actual tool simulation");
        const auto *entry = controller.session().bend(0);
        require(entry && entry->catalogTools && entry->catalogTools->die.id == die.id &&
                    entry->catalogTools->groove == 3 &&
                    entry->catalogTools->die.segments == die.segments &&
                    entry->catalogTools->punch.id == mirrored.id &&
                    entry->catalogTools->punchReversed && entry->tools->punchReversed &&
                    core::hasCompleteDrawingToolGeometry(*entry->catalogTools),
                "Graphical choice lost stock, groove or the new punch's default side");
        window.findChild<QToolButton *>("processStage1")->click();
        select(punch, offset.id, 0);
        window.findChild<QPushButton *>("applyToolSetup")->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.bendPreview() && !controller.bendPreviewError() &&
                    !controller.session().bend(0)->catalogTools->punchReversed &&
                    !controller.session().bend(0)->tools->punchReversed,
                "Changing punch kept the previous profile's default side");
        // Saved/manual orientation has priority over the new-selection preference.
        auto manual = *controller.session().bend(0)->catalogTools;
        manual.punchReversed = true;
        require(controller.setBendToolAssignment(0, manual), "Cannot set a manual side");
        window.findChild<QToolButton *>("processStage1")->click();
        window.findChild<QPushButton *>("applyToolSetup")->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.bendPreview() && !controller.bendPreviewError() &&
                    controller.session().bend(0)->catalogTools->punchReversed,
                "Applying a captured assignment overwrote its manual side");
        const auto path = folder.path() + "/selected.bend";
        controller.saveProject(native(path), files);
        wait([&] { return !controller.isSaving(); });
        require(controller.saveError().isEmpty(), "Cannot save graphical choice");
        controller.closeDocument();
        window.openStep(path);
        wait([&] { return !controller.hasWork(); });
        require(controller.session().bend(0)->catalogTools->groove == 3 &&
                    controller.session().bend(0)->catalogTools->die.segments == die.segments &&
                    controller.session().bend(0)->catalogTools->punchReversed,
                "Reload changed groove, inventory or the saved manual side");
        auto legacy = *core::toolParameters(pair);
        require(controller.setBendTools(0, legacy), "Cannot prepare a legacy assignment");
        const auto before = calculations;
        window.findChild<QToolButton *>("processStage2")->click();
        require(!controller.hasWork() && calculations == before && !controller.bendPreview() &&
                    window.findChild<QLabel *>("bendCheckSummary")->text().contains("условный"),
                "Legacy idealized parameters started a new tool simulation");
        window.findChild<QToolButton *>("processStage1")->click();
        const auto shot = qEnvironmentVariable("BENDING_TOOL_PICKER_SNAPSHOT");
        if (!shot.isEmpty())
            require(window.grab().save(shot + "-legacy.png"), "Cannot save legacy state");
        controller.closeDocument();
        window.hide();
        // A fresh window restores article IDs, not a captured project orientation.
        ui::preferences()->setValue("tools/lastPunch", QString::fromStdString(mirrored.id));
        ui::MainWindow restored({"test", "test"}, files, files, builder,
                                {pair.punch, offset, mirrored, die});
        restored.setAutomaticPartInputPrompt(false);
        restored.resize(1280, 850);
        restored.show();
        restored.raise();
        restored.activateWindow();
        restored.openStep(file);
        auto &restarted = restored.importController();
        wait([&] { return !restarted.hasWork(); });
        restored.findChild<QToolButton *>("processStage1")->click();
        restored.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        restored.findChild<QPushButton *>("applyToolSetup")->click();
        wait([&] { return !restarted.hasWork(); });
        require(restarted.bendPreview() && !restarted.bendPreviewError() &&
                    restarted.session().bend(0)->catalogTools->punch.id == mirrored.id &&
                    restarted.session().bend(0)->catalogTools->punchReversed,
                "Restoring the last article IDs lost their initial punch side");
        std::cout << "Graphical profiles, working grooves, keyboard/cancel, real-only policy and "
                     "default/manual orientations and persistence passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
