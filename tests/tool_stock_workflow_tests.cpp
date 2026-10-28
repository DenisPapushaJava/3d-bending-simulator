#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &ok) {
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(ok(), "Timeout");
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QApplication::processEvents();
}
std::filesystem::path native(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toUtf8().constData());
#endif
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No folder");
        ui::setPreferencesFile(folder.path() + "/settings.ini");
        auto pair = tool_fixture::pair();
        pair.punch.lengthMm = 835;
        pair.die.lengthMm = 835;
        auto files = std::make_shared<project::ProjectFiles>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>(),
                              {pair.punch, pair.die});
        window.setAutomaticPartInputPrompt(false);
        window.setToolProfileImport(std::make_shared<project::DxfToolImporter>(),
                                    project::writeToolCatalog);
        window.resize(1280, 850);
        window.show();
        const auto file = folder.path() + "/stock.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 210, true, 20),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture write");
        window.openStep(file);
        auto &c = window.importController();
        wait([&] { return !c.hasWork(); });
        require(bool(c.session().document()), "No document");
        window.findChild<QPushButton *>("recommendTools")->click();
        require(window.findChild<QLabel *>("toolAssemblySummary")->text().contains("835"),
                "Longer legacy tool not offered");
        QString dialogError;
        QTimer::singleShot(0, [&] {
            try {
                auto *dialog = window.findChild<QDialog *>("toolStockDialog");
                require(dialog, "No stock editor");
                auto *tool = dialog->findChild<QComboBox *>("stockTool");
                for (int index = 0; index < 2; ++index) {
                    tool->setCurrentIndex(index);
                    dialog->findChild<QPushButton *>("stockTemplate")->click();
                    auto *rows = dialog->findChild<QTableWidget *>("stockRows");
                    for (int i = 0; i < rows->rowCount(); ++i) {
                        auto *q = static_cast<QSpinBox *>(rows->cellWidget(i, 1));
                        require(q->value() == 0, "Template invented inventory");
                        if (static_cast<QDoubleSpinBox *>(rows->cellWidget(i, 0))->value() == 100)
                            q->setValue(2);
                    }
                    dialog->findChild<QPushButton *>("stockSave")->click();
                    require(dialog->findChild<QLabel *>("stockStatus")
                                ->text()
                                .contains(QStringLiteral("сохранено")),
                            "Stock not saved");
                }
                if (const auto output = qEnvironmentVariable("BENDING_STOCK_SNAPSHOT");
                    !output.isEmpty())
                    require(dialog->grab().save(output + "-library.png"), "Library snapshot");
                dialog->reject();
            } catch (const std::exception &e) {
                dialogError = QString::fromUtf8(e.what());
                if (auto *dialog = window.findChild<QDialog *>("toolStockDialog"))
                    dialog->reject();
            }
        });
        window.findChild<QPushButton *>("toolStock")->click();
        require(dialogError.isEmpty(), qPrintable(dialogError));
        window.findChild<QPushButton *>("recommendTools")->click();
        require(!window.findChild<QPushButton *>("checkBend")->isEnabled(),
                "Insufficient stock was assigned");
        window.findChild<QDoubleSpinBox *>("segmentGapLimit")->setValue(10);
        window.findChild<QPushButton *>("recommendTools")->click();
        auto *check = window.findChild<QPushButton *>("checkBend");
        require(check->isEnabled(), "Custom gap not usable");
        check->click();
        wait([&] { return !c.hasWork(); });
        if (!c.bendPreview())
            throw std::runtime_error(c.bendPreviewError() ? c.bendPreviewError()->detail
                                                          : "No press preview");
        const auto p = c.bendPreview();
        require(p->frameCount() == 61 && p->toolSegmentGaps() == std::array{10., 10.},
                "Gaps lost from report");
        const auto *entry = c.session().bend(0);
        require(entry && entry->catalogTools, "No portable assembly");
        const auto assignment = *entry->catalogTools;
        const auto &frame = cad::OcctPreviewAccess::frame(*p, 0);
        const auto full = cad::makeDrawingTools(assignment, *core::drawingToolGeometry(assignment),
                                                p->model().thicknessMm + assignment.clearanceMm);
        for (std::size_t i = 0; i < 2; ++i) {
            TopTools_IndexedMapOfShape solids;
            TopExp::MapShapes(frame.tools[i], TopAbs_SOLID, solids);
            require(solids.Extent() == 2 &&
                        std::abs(volume(frame.tools[i]) / volume(full[i]) - 200. / 210.) < 1.e-7,
                    "Tool gap was filled or pieces omitted");
        }
        if (app.arguments().contains("--graphics")) {
            window.findChild<QTabWidget *>("propertiesTabs")->setCurrentIndex(2);
            auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
            wait([&] { return viewer->isReady(); });
            if (const auto output = qEnvironmentVariable("BENDING_STOCK_SNAPSHOT");
                !output.isEmpty()) {
                require(viewer->captureImage(true).save(output + "-viewport.png"),
                        "Viewport snapshot");
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
                painter.end();
                require(shot.save(output + "-ui.png"), "Full UI snapshot");
            }
        }
        const auto project = native(folder.path() + "/assembly.bend");
        c.saveProject(project, files);
        wait([&] { return !c.hasWork(); });
        require(c.saveError().isEmpty(), "Assembly save failed");
        c.closeDocument();
        c.open(project);
        wait([&] { return !c.hasWork(); });
        require(c.session().bend(0) && c.session().bend(0)->catalogTools == assignment,
                "Stock or assembly not portable");
        QFile saved(folder.path() + "/assembly.bend");
        require(saved.open(QIODevice::ReadOnly), "Cannot read saved assembly");
        auto older = QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        require(older.value("version").toInt() == 10, "Wrong project schema for assembly");
        older.insert("version", 9);
        QFile forged(folder.path() + "/forged-v9.bend");
        require(forged.open(QIODevice::WriteOnly), "Cannot create version test");
        forged.write(QJsonDocument(older).toJson());
        forged.close();
        const auto rejected = files->load(native(forged.fileName()), {}, {});
        require(std::holds_alternative<application::ImportError>(rejected),
                "Segment assembly accepted under a legacy project version");
        QTimer::singleShot(0, [&] {
            auto *dialog = window.findChild<QDialog *>("toolStockDialog");
            auto *tool = dialog->findChild<QComboBox *>("stockTool");
            for (int index = 0; index < 2; ++index) {
                tool->setCurrentIndex(index);
                auto *rows = dialog->findChild<QTableWidget *>("stockRows");
                for (int i = 0; i < rows->rowCount(); ++i)
                    if (static_cast<QDoubleSpinBox *>(rows->cellWidget(i, 0))->value() == 100)
                        static_cast<QSpinBox *>(rows->cellWidget(i, 1))->setValue(1);
                dialog->findChild<QPushButton *>("stockSave")->click();
            }
            dialog->reject();
        });
        window.findChild<QPushButton *>("toolStock")->click();
        require(c.session().bend(0)->catalogTools == assignment,
                "Library edit rewrote a project snapshot");
        c.closeDocument();
        c.open(project);
        wait([&] { return !c.hasWork(); });
        require(c.session().bend(0)->catalogTools == assignment,
                "Project did not keep its captured inventory");
        window.openStep(file);
        wait([&] { return !c.hasWork(); });
        window.findChild<QPushButton *>("recommendTools")->click();
        require(!window.findChild<QPushButton *>("checkBend")->isEnabled(),
                "An unrelated part inherited the previous project's stock snapshot");
        if (app.arguments().contains("--sample")) {
            window.openStep(app.arguments().at(app.arguments().indexOf("--sample") + 1));
            wait([&] { return !c.hasWork(); });
            require(c.session().document() &&
                        c.session().document()->metadata().bendAnalysis.bends.size() > 1,
                    "No multi-bend sample");
            window.findChild<QTabWidget *>("propertiesTabs")->setCurrentIndex(2);
            const auto &b = c.session().document()->metadata().bendAnalysis.bends.front();
            core::PressTools conditional{
                8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8, b.lengthMm, 40};
            require(c.setBendTools(0, conditional), "Conditional sample tool assignment failed");
            require(c.prepareBendPreview(0, false,
                                         std::make_shared<cad::SingleBendPreviewBuilder>(),
                                         application::BendPreviewMode::ToolMotion),
                    "Multi-bend job not started");
            wait([&] { return !c.hasWork(); });
            if (!c.bendPreview())
                throw std::runtime_error(c.bendPreviewError() ? c.bendPreviewError()->detail
                                                              : "No sample motion");
            require(c.bendPreview()->otherFormedBends() > 0, "Multi-bend scope omitted");
            auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
            wait([&] { return viewer->isReady(); });
            if (const auto output = qEnvironmentVariable("BENDING_STOCK_SNAPSHOT");
                !output.isEmpty()) {
                require(viewer->captureImage(true).save(output + "-multi-viewport.png"),
                        "Sample screenshot failed");
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
                painter.end();
                require(shot.save(output + "-multi-ui.png"), "Sample UI screenshot failed");
            }
            c.closeDocument();
        }
        c.shutdown();
        std::cout << "Stock, custom gaps, exact tool bodies and portable assembly passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
