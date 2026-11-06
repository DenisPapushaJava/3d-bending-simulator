#include "BendFixtures.h"
#include "ToolAssemblyDialog.h"
#include "ToolFixtures.h"
#include "ToolLibraryDialog.h"
#include "ToolSelectionWidget.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toStdString());
#endif
}
QByteArray bytes(const QString &p) {
    QFile f(p);
    require(f.open(QIODevice::ReadOnly), "Read file");
    return f.readAll();
}
void write(const QString &p, const QJsonObject &root) {
    QFile f(p);
    require(f.open(QIODevice::WriteOnly), "Write file");
    const auto b = QJsonDocument(root).toJson();
    require(f.write(b) == b.size(), "Short write");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "Temporary folder");
        ui::setPreferencesFile(dir.path() + "/settings.ini");
        const auto name = dir.path() + "/tools.json";
        auto pair = tool_fixture::pair();
        pair.punch.segments =
            std::vector<core::ToolSegment>{{200, 1},
                                           {100, 1, core::SegmentShape::LeftHorn},
                                           {100, 1, core::SegmentShape::RightHorn}};
        require(!project::writeToolCatalog(native(name), {pair.punch, pair.die}),
                "Initial catalogue");
        const auto initial = bytes(name);
        application::ToolLibrary library({pair.punch, pair.die}, native(name),
                                         {project::readToolLibrary, project::writeToolLibrary});
        auto files = std::make_shared<project::ProjectFiles>();
        ui::ImportController controller(files, [](auto, auto) { return QString{}; });
        ui::ToolLibraryDialog dialog(
            library, controller, {}, [](const auto &, auto) { return QString{}; }, nullptr,
            core::ToolKind::Punch, pair.punch.id);
        dialog.resize(1100, 900);
        dialog.show();
        dialog.raise();
        dialog.activateWindow();
        QElapsedTimer exposed;
        exposed.start();
        while ((!dialog.windowHandle() || !dialog.windowHandle()->isExposed()) &&
               exposed.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        QApplication::processEvents();
        auto rows = [&] { return dialog.findChild<QTableWidget *>("stockRows"); };
        const auto configure = [&](int row) {
            rows()->selectRow(row);
            static_cast<QPushButton *>(rows()->cellWidget(row, 4))->click();
            QApplication::processEvents();
        };
        configure(1);
        auto field = [&](const char *id) { return dialog.findChild<QLineEdit *>(id); };
        require(field("hornProjection") && field("hornProjection")->text().isEmpty() &&
                    dialog.findChild<QWidget *>("hornSectionEditor")->isVisible(),
                "No direct entry or unknown horn silently initialized");
        dialog.findChild<QPushButton *>("hornUseReference")->click();
        field("hornTipHeight")->setText("9,");
        require(field("hornTipHeight")->text() == "9,", "Intermediate decimal reset");
        field("hornTipHeight")->insert("5");
        field("hornProjection")->setText("27.123456789012345");
        require(field("hornTipHeight")->text() == "9,5", "Decimal keyboard entry corrupted");
        dialog.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(library.data().tools.front().segments->at(1).horn ==
                        core::HornRelief{27.123456789012345, 27, 9.5} &&
                    !library.data().tools.front().segments->at(2).horn,
                "Saving left horn lost decimals or changed right horn");
        const auto id = library.data().tools.front().segments->at(1).variantId;
        require(!id.empty() && bytes(name + ".pre-v6.json") == initial,
                "No identity or exact upgrade backup");
        configure(1);
        require(field("hornProjection")->text().toDouble() == 27.123456789012345,
                "Opening editor rounded stored geometry");
        field("hornProjection")->setText("100");
        const auto good = bytes(name);
        dialog.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(bytes(name) == good && dialog.findChild<QLabel *>("hornEditorStatus")
                                           ->text()
                                           .contains(QStringLiteral("меньше")),
                "Invalid horn dimensions written");
        field("hornProjection")->setText("27.123456789012345");
        dialog.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(library.data().tools.front().segments->at(1).variantId == id,
                "No-op edit changed identity");
        configure(2);
        dialog.findChild<QPushButton *>("hornUseReference")->click();
        auto *diagram = dialog.findChild<QWidget *>("hornDiagram");
        QApplication::processEvents();
        diagram->repaint();
        const double scale = std::max(
            .1, std::min((diagram->width() - 140.) / 100., (diagram->height() - 80.) / 45.));
        const QPointF target((diagram->width() - 100 * scale) / 2 + (100 - 27. / 2) * scale,
                             diagram->height() - 22.);
        QMouseEvent click(QEvent::MouseButtonPress, target, target, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(diagram, &click);
        require(field("hornProjection")->hasSelectedText() &&
                    field("hornProjection")->selectedText() == "27",
                "Diagram dimension did not select its field");
        const auto output = qEnvironmentVariable("BENDING_HORN_SNAPSHOT");
        if (!output.isEmpty()) {
            QApplication::processEvents();
            require(dialog.grab().save(output), "Native editor screenshot");
        }
        dialog.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        dialog.reject();
        controller.shutdown();
        auto saved =
            std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(name)));
        require(saved.data.tools.front().segments->at(1).variantId == id &&
                    saved.data.tools.front().segments->at(2).horn == core::HornRelief{27, 27, 9},
                "Reload lost geometry");
        auto assigned = pair;
        assigned.punch = saved.data.tools.front();
        assigned.punchAssembly = core::ToolAssembly{{1, 0, 2}, 0, {}};
        assigned.die.lengthMm = 400;
        ui::ToolAssemblyDialog assembly(assigned.punch, assigned.punchAssembly, nullptr);
        auto *assemblyRows = assembly.findChild<QTableWidget *>("assemblyRows");
        require(assemblyRows->rowCount() == 3 &&
                    static_cast<QComboBox *>(assemblyRows->cellWidget(0, 0))->count() == 3,
                "Configured horns missing from manual assembly");
        assembly.findChild<QPushButton *>("assemblyApply")->click();
        require(assembly.assembly() == assigned.punchAssembly, "Manual assembly changed horn ends");
        ui::ToolSelectionWidget selection({assigned.punch, assigned.die}, nullptr);
        selection.setSelectionRequest(
            core::ToolSelectionRequest{2, std::numbers::pi / 2, 2, 250, 0}, true);
        selection.setChoice(core::toolParameters(assigned), assigned);
        selection.findChild<QPushButton *>("fitSelectedToolSections")->click();
        const auto fitted = selection.assignment();
        require(fitted && fitted->punch.id == assigned.punch.id &&
                    fitted->die.id == assigned.die.id &&
                    core::usesHornSections(fitted->punch, fitted->punchAssembly) &&
                    core::toolLength(fitted->punch, fitted->punchAssembly) == 300,
                "Quick assembly fitting ignored usable horns or changed the profiles");
        if (app.arguments().contains("--graphics")) {
            const auto file = dir.path() + "/bend.step";
            STEPControl_Writer writer;
            require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 80, true, 20),
                                    STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
                    "Graphics fixture");
            auto doc = application::resultDocument(cad::StepImporter{}.load(native(file), {}, {}));
            const auto motion = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
                doc, 0, {std::numbers::pi / 2, 2}, assigned, {});
            if (auto *e = std::get_if<application::BendPreviewError>(&motion))
                throw std::runtime_error(e->detail);
            viewer::ViewerWidget viewer;
            viewer.resize(1000, 700);
            viewer.show();
            QElapsedTimer timer;
            timer.start();
            while (!viewer.isReady() && timer.elapsed() < 10000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            require(viewer.isReady(), "Native rendering unavailable");
            require(
                viewer.presentDocument(doc, 1).isEmpty() &&
                    viewer.presentBendPreview(std::get<application::BendPreviewPtr>(motion), 60, 1)
                        .isEmpty(),
                "Actual configured horn preview failed");
            viewer.setOrientation(viewer::ViewerWidget::Orientation::Isometric);
            viewer.fitAll();
            QApplication::processEvents();
            require(viewer.captureImage().save(dir.path() + "/horn-scene.png"),
                    "Framebuffer capture");
            if (!output.isEmpty())
                require(viewer.captureImage().save(output + "-scene.png"), "Native scene snapshot");
        }
        const auto root = QJsonDocument::fromJson(bytes(name)).object();
        require(root.value("version").toInt() == 6, "Extended catalogue version missing");
        // Reader cannot silently erase a declared feature or accept malformed variants.
        for (int test = 0; test < 7; ++test) {
            auto bad = root;
            auto tools = bad.value("tools").toArray();
            auto p = tools[0].toObject();
            auto stock = p.value("segments").toArray();
            auto horn = stock[1].toObject();
            auto relief = horn.value("hornRelief").toObject();
            if (test == 0)
                bad.insert("version", 5);
            if (test == 1)
                relief.insert("type", "unknown");
            if (test == 2)
                relief.insert("radiusMm", "27");
            if (test == 3)
                horn.remove("variantId");
            if (test == 4)
                horn.insert("shape", "straight");
            if (test == 5)
                horn.insert("variantId", "bad/id");
            if (test == 6)
                relief.insert("tipHeightMm", 100);
            horn.insert("hornRelief", relief);
            stock[1] = horn;
            p.insert("segments", stock);
            tools[0] = p;
            bad.insert("tools", tools);
            write(name, bad);
            require(std::holds_alternative<std::string>(project::readToolLibrary(native(name))),
                    "Malformed horn catalogue accepted");
        }
        write(name, root);
        application::ToolLibrary reopened(saved.data.tools, native(name),
                                          {project::readToolLibrary, project::writeToolLibrary});
        require(reopened.archive(pair.punch.id).empty(), "Archive configured horn");
        const auto archived =
            std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(name)));
        require(archived.data.archived.front() == saved.data.tools.front(),
                "Archive lost horn parameters");
        std::cout << "Horn library entry, decimals, atomic save, identity, backup, reload and "
                     "archive passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
