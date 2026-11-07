#include "BendFixtures.h"
#include "ProjectFormatFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/core/ToolSelection.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
void pump() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void wait(const std::function<bool()> &f) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < 90000) {
        pump();
        QThread::msleep(2);
    }
    require(f(), "Timeout");
    pump();
}
std::filesystem::path native(const QString &s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toUtf8().constData());
#endif
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        QTemporaryDir temp;
        require(temp.isValid(), "No temp folder");
        ui::setPreferencesFile(temp.path() + "/settings.ini");
        auto die = tool_fixture::fourWayDie();
        require(die.grooveSettings.size() == 4 && core::validToolPreset(die),
                "Four-way detection failed");
        const auto file = temp.path() + "/part.step";
        STEPControl_Controller::Init();
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 40, true, 40),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture write failed");
        core::ToolCatalog catalog{tool_fixture::pair().punch, die};
        if (args.contains("--catalog")) {
            auto loaded = project::readToolCatalog(native(args.at(args.indexOf("--catalog") + 1)));
            require(std::holds_alternative<core::ToolCatalog>(loaded), "Private catalogue load");
            catalog = std::get<core::ToolCatalog>(std::move(loaded));
            const auto id = args.at(args.indexOf("--tool-id") + 1).toStdString();
            auto it = std::find_if(catalog.begin(), catalog.end(),
                                   [&](const auto &t) { return t.id == id; });
            require(it != catalog.end(), "Target tool missing");
            die = *it;
            if (args.contains("--dxf")) {
                project::DxfToolImporter reader;
                const auto result = reader.run(native(args.at(args.indexOf("--dxf") + 1)), {});
                require(std::holds_alternative<application::ToolProfileFile>(result),
                        "DXF read failed");
                const auto &f = std::get<application::ToolProfileFile>(result);
                require(f.contours.size() == 1 && f.millimetersPerUnit == 1,
                        "Ambiguous source/units");
                die.drawing = f.contours.front().drawing;
                die.drawing->millimeters = true;
                die.openingsMm.clear();
                die.grooveSettings.clear();
                for (const auto &g : core::detectDieGrooves(*die.drawing)) {
                    die.openingsMm.push_back(g.openingMm);
                    die.grooveSettings.push_back(g.settings);
                }
                require(die.grooveSettings.size() == 4, "Private four-way recognition failed");
                die.angleRadians = die.grooveSettings.front().angleRadians;
                *it = die;
            }
        }
        require(core::validToolPreset(die) && die.grooveSettings.size() == 4 &&
                    die.openingsMm.size() == 4,
                "Target record lacks four validated groove settings");
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        auto imported = files->load(native(file), {}, {});
        const auto doc = application::resultDocument(imported);
        require(doc != nullptr, "Fixture load failed");
        std::size_t checked = 0;
        for (const auto &p : catalog) {
            if (p.kind != core::ToolKind::Punch || !p.drawing || !p.parametersConfirmed)
                continue;
            for (std::size_t g = 0; g < die.openingsMm.size(); ++g) {
                core::ToolAssignment a{p, die, g, 3, "", true};
                const auto parameters = core::toolParameters(a);
                require(parameters && std::abs(parameters->dieAngleRadians -
                                               die.grooveSettings[g].angleRadians) < 1e-12,
                        "Wrong active angle");
                require(core::drawingToolGeometry(a).has_value(),
                        "Active rotated contact not recognized");
                if (!args.contains("--skip-motion")) {
                    const auto motion = builder->buildAssignedToolMotion(
                        doc, 0, core::BendTarget{std::numbers::pi / 2, 2}, a, {});
                    if (const auto *error = std::get_if<application::BendPreviewError>(&motion))
                        throw std::runtime_error(p.code + " / V" +
                                                 std::to_string(die.openingsMm[g]) + ": " +
                                                 error->detail);
                    const auto preview = std::get<application::BendPreviewPtr>(motion);
                    require(preview->usesDrawingDie() && preview->usesDrawingPunch() &&
                                preview->frameCount() == 61 && preview->punchRetraction(),
                            "Missing full motion/report");
                }
                ++checked;
                std::cout << p.code << " / V " << die.openingsMm[g] << " angle "
                          << parameters->dieAngleRadians * 180 / std::numbers::pi << " rotation "
                          << die.grooveSettings[g].rotationRadians * 180 / std::numbers::pi
                          << (args.contains("--skip-motion") ? " contact OK\n" : " motion OK\n");
            }
        }
        require(checked >= 4, "No drawing punches checked");
        const auto ownPath = native(temp.path() + "/catalog.json");
        require(!project::writeToolCatalog(ownPath, catalog), "Catalogue save failed");
        require(std::get<core::ToolCatalog>(project::readToolCatalog(ownPath)) == catalog,
                "Groove metadata roundtrip lost");
        ui::MainWindow window({"test", "test"}, files, files, builder, catalog);
        window.setAutomaticPartInputPrompt(false);
        window.show();
        auto &c = window.importController();
        window.openStep(file);
        wait([&] { return !c.hasWork(); });
        const auto p = std::find_if(catalog.begin(), catalog.end(), [](const auto &t) {
            return t.kind == core::ToolKind::Punch && t.drawing && t.parametersConfirmed;
        });
        auto *punch = window.findChild<QComboBox *>("punchPreset");
        auto *dieBox = window.findChild<QComboBox *>("diePreset");
        auto *groove = window.findChild<QComboBox *>("dieGroove");
        punch->setCurrentIndex(punch->findData(static_cast<int>(p - catalog.begin())));
        const auto di = std::find_if(catalog.begin(), catalog.end(),
                                     [&](const auto &t) { return t.id == die.id; });
        dieBox->setCurrentIndex(dieBox->findData(static_cast<int>(di - catalog.begin())));
        require(groove->count() == 4, "Four choices missing in UI");
        // This fixture defines the punch/die only, with no compatible machine mounting.
        auto *machine = window.findChild<QCheckBox *>("demoMachine");
        require(machine, "Machine selection unavailable");
        machine->setChecked(false);
        for (int g = 0; g < 4; ++g) {
            groove->setCurrentIndex(g);
            require(groove->currentText().contains(QStringLiteral("°")), "Groove angle hidden");
        }
        auto *check = window.findChild<QPushButton *>("checkBend");
        require(check->isEnabled(), "Cannot check rotated die in UI");
        check->click();
        wait([&] { return !c.hasWork(); });
        if (const auto error = c.bendPreviewError())
            throw std::runtime_error("UI check failed: " + error->detail);
        require(c.bendPreview() != nullptr, "UI check failed");
        if (args.contains("--snapshot")) {
            pump();
            auto *v = window.findChild<viewer::ViewerWidget *>();
            wait([&] { return v->isReady(); });
            require(v->captureImage(true).save(args.at(args.indexOf("--snapshot") + 1)),
                    "Snapshot failed");
        }
        const auto projectPath = native(temp.path() + "/four.bend");
        c.saveProject(projectPath, files);
        wait([&] { return !c.hasWork(); });
        require(c.saveError().isEmpty(), "Project save failed");
        c.closeDocument();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(!c.session().error() &&
                    c.session().bend(0)->catalogTools->die.grooveSettings == die.grooveSettings &&
                    c.session().bend(0)->catalogTools->groove == 3,
                "Portable groove selection lost");
        QFile json(temp.path() + "/four.bend");
        require(json.open(QIODevice::ReadOnly), "Read project");
        auto root = project_fixture::legacyRoot(json.readAll());
        json.close();
        root.insert("version", 8);
        require(json.open(QIODevice::WriteOnly | QIODevice::Truncate), "Retag old project");
        json.write(QJsonDocument(root).toJson());
        json.close();
        const auto current = c.session().document();
        c.open(projectPath);
        wait([&] { return !c.hasWork(); });
        require(c.session().error() && c.session().document() == current,
                "Old format accepted new groove semantics");
        if (args.contains("--catalog")) {
            c.closeDocument();
            window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                            QStringLiteral("/bend-90-6mm.step"));
            wait([&] { return !c.hasWork(); });
            machine->setChecked(false);
            punch->setCurrentIndex(punch->findData(static_cast<int>(p - catalog.begin())));
            dieBox->setCurrentIndex(dieBox->findData(static_cast<int>(di - catalog.begin())));
            groove->setCurrentIndex(0);
            require(!check->isEnabled(), "V16 admitted 6 mm sheet");
            groove->setCurrentIndex(3);
            require(check->isEnabled(), "V50 blocked 6 mm sheet");
            check->click();
            wait([&] { return !c.hasWork(); });
            if (c.bendPreviewError())
                throw std::runtime_error("6 mm / V50: " + c.bendPreviewError()->detail);
            require(c.bendPreview() && c.bendPreview()->usesDrawingDie(),
                    "6 mm actual die check failed");
            std::cout << "6 mm / V50 full cycle OK\n";
        }
        c.closeDocument();
        c.shutdown();
        window.close();
        if (args.contains("--write-catalog"))
            require(!project::writeToolCatalog(native(args.at(args.indexOf("--write-catalog") + 1)),
                                               catalog),
                    "Local library write failed");
        std::cout << "Checked pairs: " << checked << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
