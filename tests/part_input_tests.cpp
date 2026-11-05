#include "PartInputDialog.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <cmath>
#include <iostream>
#include <source_location>
#include <stdexcept>

using namespace bending;
namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
void pump() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}
void wait(const std::function<bool()> &condition,
          const std::source_location where = std::source_location::current()) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 15000) {
        pump();
        QThread::msleep(2);
    }
    if (!condition())
        throw std::runtime_error("Timed out at line " + std::to_string(where.line()));
    pump();
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
class Document final : public application::Document {
  public:
    application::DocumentMetadata data{};
    std::vector<application::FaceInfo> faces_;
    const application::DocumentMetadata &metadata() const noexcept override {
        return data;
    }
    const std::vector<application::FaceInfo> &faces() const noexcept override {
        return faces_;
    }
};
class Importer final : public application::DocumentImporter {
  public:
    application::ImportResult load(const std::filesystem::path &path, std::stop_token,
                                   const application::ImportProgress &) const override {
        auto doc = std::make_shared<Document>();
        doc->data.sourcePath = path;
        if (path == "detected.step")
            doc->data.thickness = {application::ThicknessStatus::Detected, 6.00000004, 1.e-6, {}};
        else if (path == "local.step")
            doc->data.thickness.evidence = {{1, 2, 6.00000004}, {3, 4, 6.00000005}};
        else if (path == "ambiguous.step")
            doc->data.thickness = {
                application::ThicknessStatus::Ambiguous, {}, 1.e-6, {{1, 2, 6.}, {3, 4, 3.}}};
        return application::DocumentPtr(doc);
    }
};
QPushButton *button(QWidget *dialog, const char *name) {
    auto *found = dialog->findChild<QPushButton *>(name);
    check(found, "Missing button");
    return found;
}
void draftTests() {
    ui::ImportController c(std::make_shared<Importer>(), [](auto, auto) { return QString{}; });
    QWidget owner;
    owner.show();
    const auto open = [&](const char *path) {
        c.open(path);
        wait([&] { return !c.hasWork(); });
        auto *dialog = new ui::PartInputDialog(c, &owner);
        dialog->show();
        pump();
        return dialog;
    };
    auto *d = open("detected.step");
    auto *material = d->findChild<QComboBox *>("sheetMaterial");
    auto *thickness = d->findChild<QDoubleSpinBox *>("partThickness");
    check(material->currentIndex() == 0 && thickness->value() == 6. &&
              !button(d, "partInputContinue")->isEnabled(),
          "Initial data defaults were invented");
    check(d->findChild<QLabel *>("partThicknessOrigin")
              ->text()
              .contains(QStringLiteral("подтверждена")),
          "Detected origin missing");
    material->setCurrentIndex(1);
    thickness->setValue(3.);
    check(
        !button(d, "partInputContinue")->isEnabled() &&
            d->findChild<QLabel *>("partInputError")->text().contains(QStringLiteral("отличается")),
        "Conflict not shown inline");
    check(!c.session().modified(), "Draft edited session before acceptance");
    button(d, "partAutomaticThickness")->click();
    const auto edit = c.session().editRevision();
    int notifications = 0;
    const auto connection =
        QObject::connect(&c, &ui::ImportController::stateChanged, [&] { ++notifications; });
    button(d, "partInputContinue")->click();
    pump();
    QObject::disconnect(connection);
    check(notifications == 1 && c.session().editRevision() == edit + 1 &&
              !c.session().manualThicknessMm() &&
              c.session().document()->metadata().thickness.thicknessMm == 6.00000004,
          "Acceptance lost precision, origin or atomicity");
    d = new ui::PartInputDialog(c, &owner);
    d->show();
    d->findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(3);
    d->findChild<QDoubleSpinBox *>("partThickness")->setValue(6.000001);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(d, &escape);
    pump();
    check(c.session().editRevision() == edit + 1 && !c.session().manualThicknessMm() &&
              c.session().unfoldSettings().material == core::SheetMaterial::MildSteel,
          "Cancel leaked draft values");
    d = open("local.step");
    check(d->findChild<QLabel *>("partThicknessOrigin")
              ->text()
              .contains(QStringLiteral("Предварительное")),
          "Local origin claimed constant thickness");
    d->findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(1);
    d->findChild<QDoubleSpinBox *>("partThickness")->setValue(3.);
    check(d->findChild<QLabel *>("partThicknessOrigin")
              ->text()
              .contains(QStringLiteral("отличается")),
          "Local discrepancy was hidden");
    button(d, "partAutomaticThickness")->click();
    button(d, "partInputContinue")->click();
    pump();
    check(!c.session().manualThicknessMm() && c.session().document()->metadata().thickness.status ==
                                                  application::ThicknessStatus::Unsupported,
          "Local proposal was upgraded to a proof");
    for (const auto *path : {"ambiguous.step", "missing.step"}) {
        d = open(path);
        check(d->findChild<QDoubleSpinBox *>("partThickness")->value() == 0.,
              "Ambiguous/absent thickness chose one distance");
        d->findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(1);
        check(!button(d, "partInputContinue")->isEnabled(), "Missing thickness accepted");
        button(d, "partInputViewOnly")->click();
        pump();
        check(c.session().document() && !c.session().modified(),
              "View-only lost model or edited settings");
    }
    d = new ui::PartInputDialog(c, &owner);
    d->show();
    d->findChild<QDoubleSpinBox *>("partThickness")->setValue(2.);
    d->findChild<QCheckBox *>("customUnfoldK")->setChecked(true);
    d->findChild<QDoubleSpinBox *>("unfoldK")->setValue(.42);
    check(button(d, "partInputContinue")->isEnabled(),
          "Expert mode requires a fictitious material");
    button(d, "partInputContinue")->click();
    pump();
    check(c.session().hasPartInputs() &&
              c.session().unfoldSettings().material == core::SheetMaterial::Unknown &&
              c.session().unfoldSettings().manualK == .42,
          "Expert mode failed");
    QPointer<QDialog> stale = new ui::PartInputDialog(c, &owner);
    stale->show();
    c.setManualThickness(2.5);
    pump();
    check(!stale && c.session().manualThicknessMm() == 2.5, "Stale edit retained live dialog");
    stale = new ui::PartInputDialog(c, &owner);
    stale->show();
    c.open("local.step");
    pump();
    check(!stale, "New import retained old dialog");
    c.cancel();
    wait([&] { return !c.hasWork(); });
    c.shutdown();
}
void workflowTests(const QString &snapshot) {
    auto files = std::make_shared<project::ProjectFiles>();
    ui::MainWindow window({"test", "test"}, files, files);
    window.setFlatPatternBuilder(std::make_shared<cad::FlatPatternBuilder>());
    window.resize(1280, 850);
    window.show();
    auto &c = window.importController();
    const auto dialog = [&] { return window.findChild<QDialog *>("partInputDialog"); };
    const auto sample = std::filesystem::path(BENDING_EXAMPLE_DIR) / "bend-90-6mm.step";
    c.open(sample);
    wait([&] { return !c.hasWork() && dialog(); });
    check(dialog()->isVisible() && dialog()->windowModality() == Qt::WindowModal &&
              dialog()->findChild<QDoubleSpinBox *>("partThickness")->value() == 6.,
          "Automatic window-modal prompt missing or has old 2 mm");
    if (!snapshot.isEmpty()) {
        check(dialog()->grab().save(snapshot), "Cannot save form snapshot");
        auto *advanced = dialog()->findChild<QToolButton *>("partInputAdvanced");
        advanced->setChecked(true);
        pump();
        check(dialog()->grab().save(snapshot + "-advanced.png"), "Cannot save expanded form");
        advanced->setChecked(false);
    }
    button(dialog(), "partInputViewOnly")->click();
    pump();
    check(!dialog() && c.session().document() && !c.session().modified(), "View-only altered STEP");
    // Missing data action reuses the same form.
    button(&window, "showFlatPattern")->click();
    check(dialog(), "Flat action did not request missing inputs");
    dialog()->findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(1);
    button(dialog(), "partInputContinue")->click();
    pump();
    check(c.session().hasPartInputs() && !c.session().manualThicknessMm(), "Form not committed");
    QTemporaryDir dir;
    check(dir.isValid(), "No temp folder");
    const auto path = native(dir.path()) / "part.bend";
    c.saveProject(path, files);
    wait([&] { return !c.hasWork(); });
    check(c.saveError().isEmpty(), "Cannot save form values");
    c.closeDocument();
    c.open(path);
    wait([&] { return !c.hasWork(); });
    check(!dialog() && c.session().hasPartInputs() && !c.session().modified(),
          "Complete project prompted again");
    c.setUnfoldSettings({core::SheetMaterial::Unknown, .42, 1});
    c.saveProject(path, files);
    wait([&] { return !c.hasWork(); });
    c.closeDocument();
    c.open(path);
    wait([&] { return !c.hasWork(); });
    check(!dialog() && c.session().hasPartInputs() &&
              c.session().unfoldSettings().material == core::SheetMaterial::Unknown &&
              c.session().unfoldSettings().manualK == .42,
          "Complete expert project was forced to choose a material");
    // Legacy projects must request their missing material.
    QFile file(dir.path() + "/part.bend");
    check(file.open(QIODevice::ReadOnly), "Cannot read project");
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    root.insert("version", 5);
    root.remove("unfold");
    auto legacyBends = root.value("bends").toArray();
    for (qsizetype i = 0; i < legacyBends.size(); ++i) {
        auto bend = legacyBends[i].toObject();
        bend.remove("workpieceReversed"); // Introduced in v14; not a valid v5 input.
        bend.remove("workpieceOffsetMm");
        legacyBends[i] = bend;
    }
    root.insert("bends", legacyBends);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write legacy fixture");
    file.write(QJsonDocument(root).toJson());
    file.close();
    c.open(path);
    wait([&] { return !c.hasWork() && dialog(); });
    check(dialog()->findChild<QComboBox *>("sheetMaterial")->currentIndex() == 0,
          "Legacy project inherited material");
    button(dialog(), "partInputViewOnly")->click();
    pump();
    // A failed load preserves the document without a repeat prompt.
    const auto previous = c.session().document();
    c.open(native(dir.path()) / "missing.step");
    wait([&] { return !c.hasWork(); });
    check(!dialog() && c.session().document() == previous && c.session().error(),
          "Failed import prompted or replaced model");
    // Replacing an in-flight import produces one prompt for the latest accepted STEP.
    c.open(sample);
    c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "plate-mm.step");
    wait([&] { return !c.hasWork() && dialog(); });
    check(window.findChildren<QDialog *>("partInputDialog").size() == 1 &&
              c.session().document()->metadata().sourcePath.filename() == "plate-mm.step",
          "Replaced imports queued stale prompts");
    c.closeDocument();
    pump();
    check(!dialog() && !c.session().document(), "Close retained a form");
    c.open(sample);
    c.cancel();
    wait([&] { return !c.hasWork(); });
    check(!dialog(), "Cancelled import opened a form");
    c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "wall-panel-2700mm.step");
    wait([&] { return !c.hasWork() && dialog(); });
    check(std::abs(dialog()->findChild<QDoubleSpinBox *>("partThickness")->value() - 1.5) < 1e-6 &&
              !c.session().manualThicknessMm() &&
              dialog()
                  ->findChild<QLabel *>("partThicknessOrigin")
                  ->text()
                  .contains(QStringLiteral("радиусам гибов")),
          "Sixteen-bend panel did not receive an automatic local thickness proposal");
    if (!snapshot.isEmpty())
        check(dialog()->grab().save(snapshot + "-panel.png"), "Cannot save panel form");
    dialog()->findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(1);
    button(dialog(), "partInputContinue")->click();
    pump();
    check(c.session().hasPartInputs() && !c.session().manualThicknessMm(),
          "Automatic proposal was saved as an invented manual thickness");
    const auto panelProject = native(dir.path()) / "panel.bend";
    c.saveProject(panelProject, files);
    wait([&] { return !c.hasWork(); });
    check(c.saveError().isEmpty() && c.session().projectPath() == panelProject,
          "Cannot save panel inputs");
    c.closeDocument();
    c.open(panelProject);
    wait([&] { return !c.hasWork(); });
    const auto panelThickness =
        application::suggestThickness(c.session().document()->metadata().thickness);
    check(!dialog() && !c.session().manualThicknessMm() && c.session().hasPartInputs() &&
              c.session().projectPath() == panelProject && panelThickness &&
              std::abs(*panelThickness - 1.5) < 1e-6,
          "Panel project lost its recomputed automatic thickness");
    c.shutdown();
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        draftTests();
        workflowTests(app.arguments().size() > 1 ? app.arguments().at(1) : QString{});
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
