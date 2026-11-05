#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void pump() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer t;
    t.start();
    while (!condition() && t.elapsed() < 60000) {
        pump();
        QThread::msleep(2);
    }
    require(condition(), "Timeout");
    pump();
}
std::filesystem::path native(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toUtf8().constData());
#endif
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        const bool graphics = args.contains("--graphics");
        QTemporaryDir dir;
        require(dir.isValid(), "No folder");
        ui::setPreferencesFile(dir.path() + "/settings.ini");
        const auto file = dir.path() + "/part.step";
        STEPControl_Controller::Init();
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 40, true, 20),
                                STEPControl_AsIs) == IFSelect_RetDone,
                "Fixture transfer");
        require(writer.Write(file.toUtf8().constData()) == IFSelect_RetDone, "Fixture write");
        auto tools = tool_fixture::pair();
        auto wider = tool_fixture::explicitPair(
            core::PressTools{50, std::numbers::pi / 3, std::numbers::pi / 3, .8, 80, 3});
        wider.die.id = "own-v50";
        wider.die.code = "V50";
        auto unsupported = tool_fixture::pair(46).die;
        unsupported.id = "own-unsupported-v48";
        unsupported.code = "V48-unsupported";
        // Better V/t score, but the groove does not match the confirmed opening.
        unsupported.openingsMm = {48};
        core::ToolCatalog catalog{tools.punch, tools.die, wider.die, unsupported};
        if (args.contains("--catalog")) {
            const auto loaded =
                project::readToolCatalog(native(args.at(args.indexOf("--catalog") + 1)));
            require(std::holds_alternative<core::ToolCatalog>(loaded), "Private catalog load");
            catalog = std::get<core::ToolCatalog>(loaded);
            const auto p = std::find_if(catalog.begin(), catalog.end(),
                                        [](const auto &t) { return t.id == "s-tool-st1405s"; });
            const auto d = std::find_if(catalog.begin(), catalog.end(),
                                        [](const auto &t) { return t.id == "s-tool-st3412s"; });
            require(p != catalog.end() && d != catalog.end(), "Missing private profiles");
            tools = {*p, *d, 0, 3, "", true};
            catalog = {*p, *d, wider.die, unsupported};
        }
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        ui::MainWindow window({"test", "test"}, files, files, builder, catalog);
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        auto &c = window.importController();
        window.openStep(file);
        wait([&] { return !c.hasWork(); });
        require(c.session().document() != nullptr, "No document");
        auto *p = window.findChild<QComboBox *>("punchPreset"),
             *d = window.findChild<QComboBox *>("diePreset");
        p->setCurrentIndex(p->findData(0));
        d->setCurrentIndex(d->findData(1));
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        auto *real = window.findChild<QCheckBox *>("useDrawingToolGeometry");
        require(real && real->isEnabled() && real->isChecked(),
                "Explicit tool choice did not activate its known shape");
        real->setChecked(true);
        auto *check = window.findChild<QPushButton *>("checkBend");
        require(check && check->isEnabled(), "Cannot check real pair");
        check->click();
        wait([&] { return !c.hasWork(); });
        if (c.bendPreviewError())
            throw std::runtime_error(c.bendPreviewError()->detail);
        require(c.bendPreview() && c.bendPreview()->usesDrawingTools() &&
                    c.session().bend(0)->catalogTools->useDrawingGeometry,
                "Real mode did not reach builder");
        auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
        if (graphics) {
            wait([&] { return viewer->isReady(); });
            require(!viewer->captureImage().isNull(), "No native image");
            if (args.contains("--snapshot"))
                require(viewer->captureImage(true).save(args.at(args.indexOf("--snapshot") + 1) +
                                                        "-ready.png"),
                        "Snapshot failed");
        }
        require(c.prepareBendPreview(0, false, builder, application::BendPreviewMode::ToolSetup),
                "Cannot prepare real static setup");
        wait([&] { return !c.hasWork(); });
        require(c.bendPreview() && c.bendPreview()->usesDrawingTools() &&
                    c.bendPreview()->frameCount() == 1 && !c.bendPreview()->pressMotion(),
                "Static controller route changed geometry mode");
        const auto projectPath = native(dir.path()) / "tool.bend";
        c.saveProject(projectPath, files);
        wait([&] { return !c.hasWork(); });
        require(c.saveError().isEmpty(), "Save failed");
        c.closeDocument();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(c.session().bend(0)->catalogTools->useDrawingGeometry &&
                    window.findChild<QCheckBox *>("useDrawingToolGeometry")->isChecked(),
                "Portable mode lost");
        QFile json(dir.path() + "/tool.bend");
        require(json.open(QIODevice::ReadOnly), "Read saved");
        auto root = QJsonDocument::fromJson(json.readAll()).object();
        json.close();
        require(root.value("version").toInt() == 17, "Wrong current project version");
        {
            auto legacyBends = root["bends"].toArray();
            for (qsizetype i = 0; i < legacyBends.size(); ++i) {
                auto legacyBend = legacyBends[i].toObject();
                legacyBend.remove("workpieceReversed");
                legacyBend.remove("workpieceOffsetMm");
                for (const auto *key : {"tools", "catalogTools"}) {
                    if (!legacyBend.value(key).isObject())
                        continue;
                    auto legacyTools = legacyBend.value(key).toObject();
                    legacyTools.remove("backgauge");
                    legacyBend[key] = legacyTools;
                }
                legacyBends[i] = legacyBend;
            }
            root["bends"] = legacyBends;
        }
        root.insert("version", 7);
        require(json.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write full v7");
        json.write(QJsonDocument(root).toJson());
        json.close();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(!c.session().error() && c.session().bend(0)->catalogTools->useDrawingGeometry,
                "Full drawing pair from version 7 lost compatibility");
        root.insert("version", 6);
        require(json.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write legacy");
        json.write(QJsonDocument(root).toJson());
        json.close();
        const auto doc = c.session().document();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(c.session().error() && c.session().document() == doc,
                "Legacy format admitted real mode");
        auto bends = root["bends"].toArray();
        auto bend = bends[0].toObject();
        auto assignment = bend["catalogTools"].toObject();
        assignment.remove("useDrawingGeometry");
        bend["catalogTools"] = assignment;
        bends[0] = bend;
        root["bends"] = bends;
        require(json.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write old schema");
        json.write(QJsonDocument(root).toJson());
        json.close();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(!c.session().error() && !c.session().bend(0)->catalogTools->useDrawingGeometry &&
                    !real->isChecked(),
                "Old project upgraded tool mode");
        require(QMetaObject::invokeMethod(p, "activated", Q_ARG(int, p->currentIndex())) &&
                    real->isChecked(),
                "Explicit reselection of same legacy tool did not activate known shape");
        auto blocked = tool_fixture::pair(12, .8, 100);
        require(c.setBendToolAssignment(0, blocked), "Blocked assignment");
        require(c.prepareBendPreview(0, false, builder, application::BendPreviewMode::ToolMotion),
                "Cannot run collision");
        wait([&] { return !c.hasWork(); });
        require(!c.bendPreview() && c.bendPreviewError() && c.bendPreviewError()->diagnostic,
                "Failed state was lost or became success");
        require(!window.findChild<QPushButton *>("playBend")->isEnabled(),
                "Collision became playable success");
        const auto diagnostic = c.bendPreviewError()->diagnostic;
        const auto edit = c.session().editRevision();
        window.findChild<QPushButton *>("returnToCad")->click();
        window.findChild<QPushButton *>("partPrimaryAction")->click();
        require(!c.hasWork() && c.bendPreviewError()->diagnostic == diagnostic &&
                    c.session().editRevision() == edit,
                "Return to diagnostic reran CAD or edited project");
        if (graphics) {
            pump();
            require(!viewer->captureImage().isNull(), "Missing collision image");
            if (args.contains("--snapshot"))
                require(viewer->captureImage(true).save(args.at(args.indexOf("--snapshot") + 1) +
                                                        "-collision.png"),
                        "Collision snapshot failed");
        }
        c.closeDocument();
        c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "bend-90-6mm.step");
        wait([&] { return !c.hasWork(); });
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        p->setCurrentIndex(p->findData(0));
        d->setCurrentIndex(d->findData(1));
        real->setChecked(false);
        require(!check->isEnabled(), "6 mm sheet admitted V12 in UI");
        require(c.setBendToolAssignment(0, tools),
                "Manual invalid assignment not stored for diagnosis");
        require(
            !c.prepareBendPreview(0, false, builder, application::BendPreviewMode::ToolMotion) &&
                c.bendPreviewError() &&
                c.bendPreviewError()->code == application::BendPreviewErrorCode::NarrowDieOpening,
            "Controller bypassed V/thickness guard");
        real->setChecked(false);
        window.findChild<QAction *>("recommendToolsQuick")->trigger();
        require(d->currentData().toInt() == 2 && check->isEnabled(),
                "6 mm UI advice did not select available V50");
        require(real->isChecked(), "Advice kept known punch as an idealized wedge");
        check->click();
        wait([&] { return !c.hasWork(); });
        if (c.bendPreviewError())
            throw std::runtime_error("Real UI motion: " + c.bendPreviewError()->detail);
        require(c.bendPreview() && c.bendPreview()->usesDrawingPunch() &&
                    c.bendPreview()->usesDrawingDie(),
                "Complete real pair was replaced with generated geometry");
        const auto scope = window.findChild<QLabel *>("bendCheckScope")->text();
        require(scope.contains(QStringLiteral("Пуансон: по DXF")) &&
                    scope.contains(QStringLiteral("Матрица: по DXF")),
                "Real scope hidden");
        if (graphics && args.contains("--snapshot"))
            require(viewer->captureImage(true).save(args.at(args.indexOf("--snapshot") + 1) +
                                                    "-wide.png"),
                    "Wide pair snapshot failed");
        // Legacy partial assignments remain portable, but cannot launch a new UI tool check.
        auto mixed = *c.session().bend(0)->catalogTools;
        mixed.die.drawing.reset();
        require(c.setBendToolAssignment(0, mixed), "Cannot retain legacy partial assignment");
        c.saveProject(projectPath, files);
        wait([&] { return !c.hasWork(); });
        require(c.saveError().isEmpty(), "Legacy partial save failed");
        c.closeDocument();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(!c.session().error() && c.session().bend(0)->catalogTools->useDrawingGeometry &&
                    !c.session().bend(0)->catalogTools->die.drawing && real->isChecked(),
                "Mixed project not portable");
        window.findChild<QToolButton *>("processStage2")->click();
        require(!c.hasWork() && !c.bendPreview() &&
                    window.findChild<QLabel *>("bendCheckSummary")->text().contains("DXF"),
                "Legacy partial assignment launched a generated tool check");
        require(json.open(QIODevice::ReadOnly), "Read mixed project");
        root = QJsonDocument::fromJson(json.readAll()).object();
        json.close();
        {
            auto legacyBends = root["bends"].toArray();
            for (qsizetype i = 0; i < legacyBends.size(); ++i) {
                auto legacyBend = legacyBends[i].toObject();
                legacyBend.remove("workpieceReversed");
                legacyBend.remove("workpieceOffsetMm");
                legacyBends[i] = legacyBend;
            }
            root["bends"] = legacyBends;
        }
        root.insert("version", 7);
        require(json.open(QIODevice::WriteOnly | QIODevice::Truncate), "Retag mixed legacy");
        json.write(QJsonDocument(root).toJson());
        json.close();
        const auto current = c.session().document();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(c.session().error() && c.session().document() == current,
                "Version 7 accepted unsupported mixed semantics");
        c.closeDocument();
        c.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
