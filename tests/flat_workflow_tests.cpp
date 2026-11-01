#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace bending;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}
QString qtPath(const std::filesystem::path &value) {
#ifdef _WIN32
    return QString::fromStdWString(value.native());
#else
    return QString::fromUtf8(value.native());
#endif
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    check(condition(), "Operation timed out");
}
class LateBuilder final : public application::FlatPatternBuilder {
  public:
    mutable std::atomic<bool> entered = false, release = false;
    class Snapshot final : public application::FlatPattern {
      public:
        application::FlatPatternInfo data;
        const application::FlatPatternInfo &info() const noexcept override {
            return data;
        }
    };
    application::FlatPatternResult build(application::DocumentPtr, application::FlatPatternRequest,
                                         std::stop_token) const override {
        entered = true;
        while (!release)
            QThread::msleep(1);
        return application::FlatPatternPtr(std::make_shared<Snapshot>());
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const bool graphics = app.arguments().contains(QStringLiteral("--graphics"));
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::FlatPatternBuilder>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>());
        window.setFlatPatternBuilder(builder);
        window.resize(1280, 850);
        window.show();
        const auto sample =
            app.arguments().contains(QStringLiteral("--sample"))
                ? app.arguments().at(app.arguments().indexOf(QStringLiteral("--sample")) + 1)
                : QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step";
        window.openStep(sample);
        auto &c = window.importController();
        wait([&] { return !c.hasWork(); });
        check(c.session().document() != nullptr, "No document");
        wait([&] { return window.findChild<QDialog *>("partInputDialog") != nullptr; });
        auto *material = window.findChild<QComboBox *>("sheetMaterial");
        auto *button = window.findChild<QPushButton *>("showFlatPattern");
        auto *custom = window.findChild<QCheckBox *>("customUnfoldK");
        auto *k = window.findChild<QDoubleSpinBox *>("unfoldK");
        check(material && button && custom && k && material->currentIndex() == 0,
              "Missing material controls or silently selected material");
        check(custom->isChecked() && k->value() == .45 && !c.session().unfoldSettings().manualK &&
                  !c.session().modified() &&
                  !window.findChild<QPushButton *>("partInputContinue")->isEnabled(),
              "New-part K default was not a pending 0.45 or skipped material confirmation");
        material->setCurrentIndex(1);
        window.findChild<QPushButton *>("partInputContinue")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(c.session().modified() &&
                  c.session().unfoldSettings().material == core::SheetMaterial::MildSteel &&
                  c.session().unfoldSettings().manualK == .45,
              "Material not edited in project");
        button->click();
        wait([&] { return !c.hasWork(); });
        if (!c.flatPattern())
            throw std::runtime_error(c.flatPatternError() ? c.flatPatternError()->detail
                                                          : "No flat snapshot");
        const auto first = c.flatPattern();
        check(!first->info().bends.empty() &&
                  std::all_of(first->info().bends.begin(), first->info().bends.end(),
                              [](const auto &bend) { return bend.k == .45; }),
              "Default 0.45 did not reach the actual flat-pattern calculation");
        const auto edit = c.session().editRevision();
        button->click();
        check(c.flatPattern() == first && !c.hasWork() && edit == c.session().editRevision(),
              "Show recomputed flat or edited project");
        auto *tabs = window.findChild<QTabWidget *>("propertiesTabs");
        tabs->setCurrentIndex(2);
        auto *checkFlat = window.findChild<QPushButton *>("checkFlatPattern");
        checkFlat->click();
        check(c.flatPattern() == first && !c.hasWork(), "Check flow recomputed flat");
        auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
        if (graphics) {
            wait([&] { return viewer->isReady(); });
            check(viewer->dimensionsVisible(), "Flat dimensions are not shown");
            const auto *dimensions = viewer->findChild<QToolButton *>("showDimensions");
            check(dimensions != nullptr, "Missing dimensions control");
            const auto labels = dimensions->accessibleDescription().split(QLatin1Char('\n'));
            int coordinates = 0, callouts = 0;
            for (const auto &label : labels) {
                if (!label.startsWith(QLatin1Char('B')))
                    continue;
                if (label.contains(QChar(0x2191)) || label.contains(QChar(0x2193)))
                    ++callouts;
                else
                    ++coordinates;
            }
            check(callouts == static_cast<int>(first->info().bends.size()),
                  "Flat annotations lost individual bend IDs");
            if (app.arguments().contains(QStringLiteral("--expect-coordinate-rows"))) {
                bool ok = false;
                const auto expected =
                    app.arguments()
                        .at(app.arguments().indexOf(QStringLiteral("--expect-coordinate-rows")) + 1)
                        .toInt(&ok);
                check(ok && coordinates == expected,
                      "Coincident bend coordinates were not grouped");
            }
            wait([&] { return !viewer->captureImage().isNull(); });
            if (const auto output = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT");
                !output.isEmpty()) {
                check(viewer->captureImage(true).save(output + "-viewport.png"),
                      "Viewport capture failed");
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint(0, 0)), viewer->captureImage(true));
                painter.end();
                check(shot.save(output + "-ui.png"), "Window capture failed");
                tabs->setCurrentIndex(0);
                QApplication::processEvents();
                auto part = window.grab().toImage();
                QPainter partPainter(&part);
                partPainter.drawImage(viewer->mapTo(&window, QPoint(0, 0)),
                                      viewer->captureImage(true));
                partPainter.end();
                check(part.save(output + "-part.png"), "Part panel capture failed");
                tabs->setCurrentIndex(2);
            }
            const auto revision = c.session().revision();
            check(viewer->presentFlatPattern(first, revision + 1)
                      .contains(QStringLiteral("Неактуальная")),
                  "Stale flat accepted by viewer");
            check(viewer->endBendPreview().isEmpty(), "Return to source failed");
            check(viewer->presentFlatPattern(first, revision).isEmpty(), "Return to flat failed");
            viewer->setDimensionsVisible(false);
            viewer->setDimensionsVisible(true);
        }
        window.findChild<QPushButton *>("editPartInputs")->click();
        custom = window.findChild<QCheckBox *>("customUnfoldK");
        k = window.findChild<QDoubleSpinBox *>("unfoldK");
        custom->setChecked(true);
        k->setValue(.42);
        window.findChild<QPushButton *>("partInputContinue")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!c.flatPattern() && c.session().unfoldSettings().manualK == .42,
              "Rule change retained obsolete flat");
        QTemporaryDir dir;
        check(dir.isValid(), "No temp folder");
        const auto path = native(dir.path()) / "material.bend";
        c.saveProject(path, files);
        wait([&] { return !c.hasWork(); });
        check(c.saveError().isEmpty(), "Save failed");
        c.closeDocument();
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().material == core::SheetMaterial::MildSteel &&
                  c.session().unfoldSettings().manualK == .42 && !c.session().modified(),
              "Material/K not portable");
        window.setAutomaticPartInputPrompt(false);
        // Malformed rule data is rejected before it can modify the session.
        QFile saved(qtPath(path));
        check(saved.open(QIODevice::ReadOnly), "Cannot read saved project");
        auto root = QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        // An explicitly saved material-table mode must not become the new default constant.
        auto tableRoot = root;
        auto tableParams = tableRoot.value("parameters").toObject();
        auto tableRule = tableParams.value("unfold").toObject();
        tableRule.insert("manualK", QJsonValue::Null);
        tableParams.insert("unfold", tableRule);
        tableRoot.insert("parameters", tableParams);
        const auto tablePath = native(dir.path()) / "table.bend";
        QFile tableFile(qtPath(tablePath));
        check(tableFile.open(QIODevice::WriteOnly), "Cannot write table-mode project");
        tableFile.write(QJsonDocument(tableRoot).toJson());
        tableFile.close();
        c.open(tablePath);
        wait([&] { return !c.hasWork(); });
        window.findChild<QPushButton *>("editPartInputs")->click();
        check(!window.findChild<QCheckBox *>("customUnfoldK")->isChecked() &&
                  !c.session().unfoldSettings().manualK && !c.session().modified(),
              "Saved table mode was silently replaced by K 0.45");
        window.findChild<QPushButton *>("partInputViewOnly")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().manualK == .42 && !c.session().modified(),
              "Saved explicit K was replaced by the new default");
        auto params = root.value("parameters").toObject();
        auto rule = params.value("unfold").toObject();
        rule.insert("referenceVersion", 99);
        params.insert("unfold", rule);
        root.insert("parameters", params);
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write test project");
        saved.write(QJsonDocument(root).toJson());
        saved.close();
        const auto original = c.session().document();
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().document() == original && c.session().error(),
              "Invalid material rule replaced document");
        // Legacy projects have no material; they must not inherit the previous document's rule.
        root.insert("version", 5);
        auto legacyParameters = root.value("parameters").toObject();
        legacyParameters.remove("unfold");
        root.insert("parameters", legacyParameters);
        auto legacyBends = root.value("bends").toArray();
        for (qsizetype i = 0; i < legacyBends.size(); ++i) {
            auto bend = legacyBends[i].toObject();
            bend.remove("workpieceReversed"); // v14 field is invalid in a v5 fixture.
            legacyBends[i] = bend;
        }
        root.insert("bends", legacyBends);
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "Cannot write legacy project");
        saved.write(QJsonDocument(root).toJson());
        saved.close();
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(!c.session().error() &&
                  c.session().unfoldSettings().material == core::SheetMaterial::Unknown,
              "Legacy project inherited material");
        // A builder that returns after cancellation must never publish its old result.
        auto slow = std::make_shared<LateBuilder>();
        check(c.prepareFlatPattern(slow), "Slow flat not started");
        wait([&] { return slow->entered.load(); });
        c.closeDocument();
        slow->release = true;
        wait([&] { return !c.hasWork(); });
        check(!c.flatPattern() && !c.session().document(), "Late flat result survived close");
        c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "bend-90-6mm.step");
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().material == core::SheetMaterial::Unknown &&
                  !c.session().unfoldSettings().manualK,
              "Material leaked to another STEP");
        window.findChild<QPushButton *>("editPartInputs")->click();
        check(window.findChild<QDoubleSpinBox *>("unfoldK")->value() == .45,
              "A fresh STEP did not offer the constant default");
        window.findChild<QPushButton *>("partInputViewOnly")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!c.session().unfoldSettings().manualK && !c.session().modified() &&
                  !c.session().hasPartInputs(),
              "Cancelling the default applied unconfirmed calculation inputs");
        c.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
