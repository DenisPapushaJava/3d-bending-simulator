#include "ToolFixtures.h"
#include "bending/core/ToolGeometry.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <atomic>
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
    while (!f() && t.elapsed() < 30000) {
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
QByteArray tag(int c, const QByteArray &v) {
    return QByteArray::number(c) + '\n' + v + '\n';
}
QByteArray tag(int c, double v) {
    return tag(c, QByteArray::number(v, 'g', 16));
}
QByteArray wrap(const QByteArray &entities, int units = 4) {
    return "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n" + tag(70, units) +
           "0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" + entities + "0\nENDSEC\n0\nEOF\n";
}
QByteArray poly(const core::ToolDrawing &d, const QByteArray &layer = "PROFILE") {
    QByteArray s = "0\nLWPOLYLINE\n" + tag(8, layer) +
                   tag(90, static_cast<double>(d.edges.size())) + tag(70, 1);
    for (const auto &e : d.edges)
        s += tag(10, e.start.x) + tag(20, e.start.y) + tag(42, std::tan(e.sweepRadians / 4));
    return s;
}
void write(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size(),
            "Write fixture");
}
application::ToolProfileFile load(project::DxfToolImporter &i, const QString &path) {
    auto r = i.run(native(path), {});
    if (auto *e = std::get_if<application::ToolProfileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<application::ToolProfileFile>(std::move(r));
}
bool checked(project::DxfToolImporter &i, core::ToolDrawing d) {
    d.millimeters = true;
    return std::holds_alternative<application::CheckedToolProfile>(i.run(d, {}));
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "No temp folder");
        ui::setPreferencesFile(dir.path() + "/settings.ini");
        const auto path = dir.path() + QStringLiteral("/Мой профиль.dxf");
        project::DxfToolImporter importer;
        if (app.arguments().contains("--inspect")) {
            for (int i = 2; i < app.arguments().size(); ++i) {
                auto result = importer.run(native(app.arguments().at(i)), {});
                if (const auto *e = std::get_if<application::ToolProfileError>(&result)) {
                    std::cout << QFileInfo(app.arguments().at(i)).fileName().toStdString() << ": "
                              << e->message << '\n';
                    continue;
                }
                const auto &f = std::get<application::ToolProfileFile>(result);
                std::cout << QFileInfo(app.arguments().at(i)).fileName().toStdString()
                          << ": contours=" << f.contours.size()
                          << " units=" << f.millimetersPerUnit.value_or(0) << '\n';
                for (const auto &c : f.contours)
                    std::cout << "  " << c.layer << " edges=" << c.drawing.edges.size()
                              << " solid_at_1unit_1mm=" << checked(importer, c.drawing) << '\n';
                for (const auto &n : f.notices)
                    std::cout << "  " << n << '\n';
            }
            return 0;
        }
        const auto tools = tool_fixture::pair();
        write(path, wrap(poly(*tools.punch.drawing)));
        auto parsed = load(importer, path);
        require(parsed.millimetersPerUnit == 1 && parsed.contours.size() == 1,
                "Units/contour count");
        require(checked(importer, parsed.contours[0].drawing), "Bulge body invalid");
        require(parsed.contours[0].drawing.sha256.size() == 64 &&
                    parsed.contours[0].drawing.sourceFile == "Мой профиль.dxf",
                "Provenance lost");
        const auto arc = std::find_if(parsed.contours[0].drawing.edges.begin(),
                                      parsed.contours[0].drawing.edges.end(),
                                      [](const auto &e) { return e.center.has_value(); });
        require(arc != parsed.contours[0].drawing.edges.end() &&
                    std::abs(std::abs(arc->sweepRadians) - 2 * std::numbers::pi / 3) < 1e-10,
                "Bulge was tessellated or changed");
        write(
            path,
            wrap(poly(*tools.punch.drawing) +
                     "0\nLINE\n8\nAXES\n10\n0\n20\n0\n11\n100\n21\n0\n0\nTEXT\n8\nNOTES\n1\nTest\n",
                 0));
        parsed = load(importer, path);
        require(!parsed.millimetersPerUnit && parsed.contours.size() == 1 &&
                    !parsed.notices.empty(),
                "Unknown units or omissions hidden");
        write(path, wrap(poly(*tools.punch.drawing) + poly(*tools.die.drawing, "OTHER"), 1));
        parsed = load(importer, path);
        require(parsed.millimetersPerUnit == 25.4 && parsed.contours.size() == 2,
                "Layer candidates lost");
        const auto rejected = [&](const QByteArray &b) {
            write(path, b);
            return std::holds_alternative<application::ToolProfileError>(
                importer.run(native(path), {}));
        };
        require(rejected(wrap(poly(*tools.punch.drawing) + "0\nSPLINE\n8\nPROFILE\n")),
                "Unsupported same-layer entity silently dropped");
        require(rejected(wrap("0\nINSERT\n2\nTOOL\n")), "Block interpreted as profile");
        require(rejected("AutoCAD Binary DXF\r\n"), "Binary accepted");
        require(rejected("0\nSECTION\n2\nENTITIES\n"), "Truncated DXF accepted");
        auto wrong = poly(*tools.punch.drawing);
        wrong.replace("70\n1\n", "70\n9\n");
        require(rejected(wrap(wrong)), "3D polyline flags accepted");
        wrong = poly(*tools.punch.drawing) + tag(210, 1);
        require(rejected(wrap(wrong)), "OCS ignored");
        wrong = poly(*tools.punch.drawing) + tag(43, 2);
        require(rejected(wrap(wrong)), "Polyline width ignored");
        const QByteArray bow = "0\nLWPOLYLINE\n90\n4\n70\n1\n10\n0\n20\n0\n10\n10\n20\n10\n10\n0\n2"
                               "0\n10\n10\n10\n20\n0\n";
        write(path, wrap(bow));
        parsed = load(importer, path);
        require(!checked(importer, parsed.contours[0].drawing),
                "Self-crossing contour accepted as a body");
        const QByteArray oldpoly = "0\nPOLYLINE\n70\n1\n0\nVERTEX\n10\n0\n20\n0\n0\nVERTEX\n10\n10"
                                   "\n20\n0\n0\nVERTEX\n10\n0\n20\n10\n0\nSEQEND\n";
        write(path, wrap(oldpoly));
        require(checked(importer, load(importer, path).contours[0].drawing), "Old POLYLINE failed");
        const QByteArray arcLines = "0\nARC\n10\n0\n20\n0\n40\n10\n50\n0\n51\n180\n0\nLINE\n10\n-"
                                    "10\n20\n0\n11\n10\n21\n0\n";
        write(path, wrap(arcLines));
        require(checked(importer, load(importer, path).contours[0].drawing),
                "LINE/ARC ordering failed");
        auto paper = oldpoly;
        paper.replace("0\nPOLYLINE\n", "0\nPOLYLINE\n67\n1\n");
        write(path, wrap(paper + arcLines));
        require(load(importer, path).contours.size() == 1,
                "Paper-space polyline vertices corrupted model geometry");
        const QByteArray precise = "0\nLINE\n10\n0\n20\n0\n11\n10.0000004\n21\n0\n"
                                   "0\nLINE\n10\n10\n20\n0\n11\n0\n21\n10\n"
                                   "0\nLINE\n10\n0\n20\n10\n11\n0\n21\n0\n";
        write(path, wrap(precise));
        require(checked(importer, load(importer, path).contours[0].drawing),
                "DXF rounding precision broke closure");
        auto gap = precise;
        gap.replace("10.0000004", "10.01");
        require(rejected(wrap(gap)), "Open gap was silently filled");
        write(path, wrap("0\nCIRCLE\n10\n0\n20\n0\n40\n10\n"));
        require(checked(importer, load(importer, path).contours[0].drawing), "CIRCLE failed");
        std::stop_source stop;
        stop.request_stop();
        require(std::holds_alternative<application::ToolProfileError>(
                    importer.run(native(path), stop.get_token())),
                "Cancel ignored");
        write(path, QByteArray(4 * 1024 * 1024 + 1, 'x'));
        require(
            std::holds_alternative<application::ToolProfileError>(importer.run(native(path), {})),
            "Size limit ignored");

        auto files = std::make_shared<project::ProjectFiles>();
        auto service = std::make_shared<project::DxfToolImporter>();
        ui::MainWindow window({"test", "test"}, files, files, {},
                              core::ToolCatalog{tools.punch, tools.die});
        window.setAutomaticPartInputPrompt(false);
        window.setToolProfileImport(service, {project::readToolLibrary, project::writeToolLibrary});
        window.show();
        auto &controller = window.importController();
        window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                        QStringLiteral("/bend-90-6mm.step"));
        wait([&] { return !controller.hasWork(); });
        auto *button = window.findChild<QPushButton *>("toolStock");
        require(button && button->isEnabled(), "DXF entry unavailable");
        write(path, wrap(poly(*tools.punch.drawing), 0));
        bool completed = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            require(dialog, "No import dialog");
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(path)});
            QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction | Qt::MoveAction, &mime,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dialog, &enter);
            require(enter.isAccepted(), "DXF drop rejected by the tool dialog");
            QDragMoveEvent move(QPoint(20, 20), Qt::CopyAction | Qt::MoveAction, &mime,
                                Qt::LeftButton, Qt::ShiftModifier);
            QApplication::sendEvent(dialog, &move);
            require(move.isAccepted() && move.dropAction() == Qt::CopyAction,
                    "DXF dragging requested moving the original file");
            QDropEvent drop(QPointF(20, 20), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                            Qt::ShiftModifier);
            QApplication::sendEvent(dialog, &drop);
            require(drop.isAccepted() && drop.dropAction() == Qt::CopyAction,
                    "DXF drop did not load a section safely");
            wait([&] { return !controller.hasWork(); });
            auto *save = dialog->findChild<QPushButton *>("saveImportedTool");
            require(!save->isEnabled(), "Unknown units silently treated as mm");
            dialog->findChild<QComboBox *>("importToolUnits")->setCurrentIndex(1);
            dialog->findChild<QLineEdit *>("importToolCode")
                ->setText(QStringLiteral("Мой пуансон"));
            dialog->findChild<QDoubleSpinBox *>("importToolLength")->setValue(80);
            auto *measurements = dialog->findChild<QLabel *>("importToolProfileSize");
            require(!dialog->findChild<QDoubleSpinBox *>("importToolAngle") &&
                        !dialog->findChild<QDoubleSpinBox *>("importToolRadius") &&
                        !dialog->findChild<QLineEdit *>("importToolOpenings") &&
                        measurements->text().contains("R 0.8") &&
                        measurements->text().contains("45.000"),
                    "DXF dimensions became editable or were not measured");
            require(dialog->findChild<QToolButton *>("toolImportAdvancedToggle")->isChecked(),
                    "Unknown units were hidden in collapsed settings");
            dialog->findChild<QComboBox *>("importToolUnits")->setCurrentIndex(2);
            require(measurements->text().contains("R 20.32"),
                    "DXF unit change retained a stale nose radius");
            dialog->findChild<QComboBox *>("importToolUnits")->setCurrentIndex(1);
            QMimeData foreign;
            foreign.setUrls({QUrl::fromLocalFile(dir.path() + "/part.step")});
            QDragEnterEvent foreignEnter(QPoint(20, 20), Qt::CopyAction, &foreign, Qt::LeftButton,
                                         Qt::NoModifier);
            QApplication::sendEvent(dialog, &foreignEnter);
            require(!foreignEnter.isAccepted() && !controller.hasWork(),
                    "Tool dialog accepted a non-DXF drop");
            dialog->findChild<QToolButton *>("toolImportAdvancedToggle")->setChecked(false);
            require(!dialog->findChild<QWidget *>("toolImportAdvanced")->isVisible(),
                    "Advanced import settings remain permanently visible");
            dialog->findChild<QToolButton *>("rotateToolProfileRight")->click();
            require(dialog->findChild<QDoubleSpinBox *>("importToolRotation")->value() == 90,
                    "Profile rotation button did not update its source placement");
            dialog->findChild<QToolButton *>("rotateToolProfileLeft")->click();
            auto *mirror = dialog->findChild<QCheckBox *>("importToolMirror");
            mirror->setChecked(true);
            mirror->setChecked(false);
            require(save->isEnabled(), "Complete tool not saveable");
            const auto snapshot = qEnvironmentVariable("BENDING_DXF_SNAPSHOT");
            if (!snapshot.isEmpty()) {
                wait([&] {
                    const auto *canvas = dialog->findChild<QWidget *>("toolDrawingCanvas");
                    return canvas && canvas->isVisible() && canvas->width() > 100;
                });
                require(dialog->grab().save(snapshot), "Snapshot failed");
            }
            save->click();
            wait([&] { return !controller.hasWork(); });
            QApplication::processEvents();
            dialog->findChild<QPushButton *>("libraryUse")->click();
            completed = true;
        });
        button->click();
        require(completed, "Import callback missed");
        const auto library = ui::toolCatalogPath();
        require(!library.isEmpty(), "Library path not saved");
        auto saved = project::readToolCatalog(native(library));
        require(std::holds_alternative<core::ToolCatalog>(saved), "Library unreadable");
        auto catalog = std::get<core::ToolCatalog>(saved);
        require(catalog.size() == 3 && catalog.back().parametersConfirmed &&
                    catalog.back().drawing && catalog.back().code == "Мой пуансон" &&
                    std::abs(*catalog.back().heightMm - 45) < 1.e-6 &&
                    std::abs(catalog.back().angleRadians - std::numbers::pi / 3) < 1.e-6 &&
                    std::abs(*catalog.back().noseRadiusMm - .8) < 1.e-6,
                "New tool not persisted");
        require(core::drawingToolGeometry({catalog.back(), tools.die, 0, 3, "", true}).has_value(),
                "Imported analytic nose no longer supports existing contact");
        require(window.findChild<QComboBox *>("punchPreset")
                    ->currentText()
                    .contains(QStringLiteral("Мой пуансон")),
                "New tool not selected");
        QFile::remove(path);
        require(checked(importer, *catalog.back().drawing),
                "Library depends on source DXF after import");
        const auto before = catalog;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            require(dialog, "No cancel dialog");
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        require(std::get<core::ToolCatalog>(project::readToolCatalog(native(library))) == before,
                "Cancel modified library");
        require(project::writeToolCatalog(native(dir.path()), before).has_value(),
                "Failed write not reported");
        require(std::get<core::ToolCatalog>(project::readToolCatalog(native(library))) == before,
                "Failed write damaged library");
        // A crossing section must stay in the editor and cannot change the library.
        write(path, wrap(bow));
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            QMetaObject::invokeMethod(dialog, "loadFile", Q_ARG(QString, path));
            wait([&] { return !controller.hasWork(); });
            dialog->findChild<QPushButton *>("saveImportedTool")->click();
            wait([&] { return !controller.hasWork(); });
            require(dialog->isVisible() && dialog->findChild<QLabel *>("toolImportStatus")
                                               ->text()
                                               .contains(QStringLiteral("Сечение")),
                    "Bad section closed editor as success");
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        require(std::get<core::ToolCatalog>(project::readToolCatalog(native(library))) == before,
                "Bad section modified catalogue");
        // Updating an existing record retains its identity, other grooves and neighbouring tools.
        const auto replacement = tool_fixture::explicitPair(
            {12, std::numbers::pi / 3, 26 * std::numbers::pi / 180, 1.6, 80, 10});
        write(path, wrap(poly(*replacement.punch.drawing)));
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            dialog->findChild<QListWidget *>("libraryTools")->setCurrentRow(0);
            dialog->findChild<QPushButton *>("libraryReplaceDxf")->click();
            QMetaObject::invokeMethod(dialog, "loadFile", Q_ARG(QString, path));
            wait([&] { return !controller.hasWork(); });
            dialog->findChild<QPushButton *>("saveImportedTool")->click();
            wait([&] { return !controller.hasWork(); });
            QApplication::processEvents();
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        catalog = std::get<core::ToolCatalog>(project::readToolCatalog(native(library)));
        require(catalog.size() == before.size() && catalog[0].id == before[0].id &&
                    catalog[1] == before[1] && std::abs(*catalog[0].noseRadiusMm - 1.6) < 1.e-6 &&
                    std::abs(catalog[0].angleRadians - 26 * std::numbers::pi / 180) < 1.e-6,
                "DXF replacement retained stale dimensions, duplicated a tool or lost neighbour");
        // Unknown parameters remain inventory data, rather than an assignable machine tool.
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            QMetaObject::invokeMethod(dialog, "loadFile", Q_ARG(QString, path));
            wait([&] { return !controller.hasWork(); });
            dialog->findChild<QLineEdit *>("importToolCode")
                ->setText(QStringLiteral("Только контур"));
            dialog->findChild<QPushButton *>("saveImportedTool")->click();
            wait([&] { return !controller.hasWork(); });
            QApplication::processEvents();
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        catalog = std::get<core::ToolCatalog>(project::readToolCatalog(native(library)));
        require(catalog.size() == 4 && !catalog.back().parametersConfirmed &&
                    catalog.back().drawing,
                "Incomplete tool became assignable or was lost");
        // Ambiguous multi-contour files must ask, rather than import the first body.
        write(path, wrap(poly(*tools.punch.drawing) + poly(*tools.die.drawing, "OTHER")));
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            QMetaObject::invokeMethod(dialog, "loadFile", Q_ARG(QString, path));
            wait([&] { return !controller.hasWork(); });
            auto *contours = dialog->findChild<QComboBox *>("importToolContour");
            require(contours->count() == 2 && contours->currentIndex() == -1 &&
                        !dialog->findChild<QPushButton *>("saveImportedTool")->isEnabled() &&
                        dialog->findChild<QToolButton *>("toolImportAdvancedToggle")->isChecked(),
                    "Multi-contour DXF silently selected an arbitrary body");
            contours->setCurrentIndex(0);
            require(dialog->findChild<QPushButton *>("saveImportedTool")->isEnabled(),
                    "Explicit contour selection remained unavailable");
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        write(path, wrap(poly(*tool_fixture::fourWayDie().drawing)));
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            QMetaObject::invokeMethod(dialog, "loadFile", Q_ARG(QString, path));
            wait([&] { return !controller.hasWork(); });
            auto *table = dialog->findChild<QTableWidget *>("importToolGrooves");
            require(table->rowCount() == 4 && table->item(3, 1)->text() == QStringLiteral("90"),
                    "Four-way import table missing");
            table->setCurrentCell(3, 0);
            dialog->findChild<QDoubleSpinBox *>("importToolLength")->setValue(80);
            require(dialog->findChild<QComboBox *>("importToolKind")->currentIndex() == 1 &&
                        !dialog->findChild<QDoubleSpinBox *>("importToolAngle") &&
                        !dialog->findChild<QLineEdit *>("importToolOpenings") &&
                        !dialog->findChild<QPushButton *>("recognizeToolGrooves"),
                    "Type was not inferred or manual dimensions can override DXF");
            auto *groove = dialog->findChild<QToolButton *>("importToolGroove3");
            require(groove && groove->isVisible(), "Interactive groove choices are missing");
            const auto *canvas = dialog->findChild<QWidget *>("toolDrawingCanvas");
            groove->click();
            require(canvas == dialog->findChild<QWidget *>("toolDrawingCanvas"),
                    "Working-side change recreated the preview canvas");
            require(
                table->currentRow() == 3 && groove->isChecked() &&
                    dialog->findChild<QLabel *>("importToolProfileSize")->text().contains("V 30"),
                "Groove click did not update the working profile");
            if (const auto snapshot = qEnvironmentVariable("BENDING_DXF_SNAPSHOT");
                !snapshot.isEmpty())
                require(dialog->grab().save(snapshot + "-die.png"), "Die snapshot failed");
            dialog->findChild<QPushButton *>("saveImportedTool")->click();
            wait([&] { return !controller.hasWork(); });
            QApplication::processEvents();
            QMetaObject::invokeMethod(dialog, "reject");
        });
        button->click();
        catalog = std::get<core::ToolCatalog>(project::readToolCatalog(native(library)));
        require(catalog.size() == 5 && catalog.back().grooveSettings.size() == 4 &&
                    catalog.back().parametersConfirmed,
                "Recognized settings lost on save");
        QFile libraryFile(library);
        require(libraryFile.open(QIODevice::ReadOnly), "Read catalogue version");
        auto legacyRoot = QJsonDocument::fromJson(libraryFile.readAll()).object();
        libraryFile.close();
        require(legacyRoot.value("version").toInt() == 5, "Catalogue version not advanced");
        legacyRoot.insert("version", 2);
        const auto legacyPath = dir.path() + QStringLiteral("/legacy.json");
        write(legacyPath, QJsonDocument(legacyRoot).toJson());
        require(std::holds_alternative<std::string>(project::readToolCatalog(native(legacyPath))),
                "Old catalogue accepted orientation metadata");
        // Cancellation suppresses late callbacks without changing the loaded STEP or preview.
        struct Slow final : application::ToolProfileImporter {
            mutable std::atomic_bool entered{false};
            application::ToolProfileResult run(const application::ToolProfileInput &,
                                               std::stop_token token) const override {
                entered = true;
                while (!token.stop_requested())
                    QThread::msleep(1);
                return application::ToolProfileFile{};
            }
        };
        auto slow = std::make_shared<Slow>();
        bool late = false;
        const auto document = controller.session().document();
        require(controller.prepareToolProfile(native(path), slow, [&](auto) { late = true; }),
                "Cannot start worker cancellation test");
        wait([&] { return slow->entered.load(); });
        controller.cancelToolProfile();
        wait([&] { return !controller.hasWork(); });
        require(!late && controller.session().document() == document,
                "Cancelled library job published or changed CAD document");
        QTemporaryDir moved;
        require(
            QDir().mkpath(moved.path() + QStringLiteral("/tools")) &&
                QFile::copy(library, moved.path() + QStringLiteral("/tools/tool-catalog.json")) &&
                QFile::copy(dir.path() + QStringLiteral("/settings.ini"),
                            moved.path() + QStringLiteral("/settings.ini")),
            "Prepare portable relocation");
        ui::setPreferencesFile(moved.path() + QStringLiteral("/settings.ini"));
        require(std::get<core::ToolCatalog>(
                    project::readToolCatalog(native(ui::toolCatalogPath()))) == catalog,
                "Portable library path depended on previous installation directory");
        ui::setPreferencesFile(dir.path() + QStringLiteral("/settings.ini"));
        controller.shutdown();
        window.close();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
