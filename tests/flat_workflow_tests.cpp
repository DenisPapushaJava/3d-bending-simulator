#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
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
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
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
            auto *bends = window.findChild<QListView *>("bendList");
            check(bends != nullptr, "No bend list");
            // Overview must not carry every bend's full dimensions.
            viewer->highlightFaces({}, c.session().revision());
            check(!dimensions->accessibleDescription().contains(QStringLiteral("ВВЕРХ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral("ВНИЗ")),
                  "Overview retained full bend callouts");
            const auto revisionBeforeSelection = c.session().editRevision();
            int picked = -1;
            QObject::connect(viewer, &viewer::ViewerWidget::bendSelectionChanged, &window,
                             [&](quint64, int index) { picked = index; });
            auto *canvas = viewer->findChild<QWidget *>("cadCanvas");
            const auto sendClick = [&](QPoint point) {
                QMouseEvent event(QEvent::MouseButtonPress, QPointF(point),
                                  QPointF(canvas->mapToGlobal(point)), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas, &event);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                                    QPointF(canvas->mapToGlobal(point)), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(canvas, &release);
            };
            // Pick an actual rendered orange bend line, not a hard-coded camera position.
            const auto overview = viewer->captureImage();
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty())
                overview.save(path + "-overview.png");
            bool clicked = false;
            for (int y = 30; y < overview.height() - 30 && !clicked; ++y)
                for (int x = 30; x < overview.width() - 220 && !clicked; ++x) {
                    const auto color = overview.pixelColor(x, y);
                    if (color.red() > 200 && color.green() > 70 && color.green() < 190 &&
                        color.blue() < 110) {
                        sendClick(QPoint(x, y) / canvas->devicePixelRatioF());
                        clicked = picked >= 0;
                    }
                }
            check(clicked && picked >= 0 && bends->currentIndex().row() == picked,
                  "Flat line click did not select its source bend in the list");
            check(dimensions->accessibleDescription().contains(QStringLiteral(" · L ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · Y ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · X ")) &&
                      !c.hasWork() && c.flatPattern() == first &&
                      c.session().editRevision() == revisionBeforeSelection,
                  "Flat selection recalculated CAD/edited project or omitted selected dimensions");
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty())
                viewer->captureImage().save(path + "-selected-bend.png");
            const auto topLabels = dimensions->accessibleDescription();
            const auto measuredLabels = [](const QString &description) {
                // Compact ID visibility depends on viewport fitting, while the
                // selected measurements and their datum must remain identical.
                const QRegularExpression marker(QStringLiteral("^B[0-9]+(?:/B[0-9]+)*$"));
                QStringList result;
                for (const auto &line : description.split('\n'))
                    if (!marker.match(line).hasMatch())
                        result << line;
                return result;
            };
            const bool up = topLabels.contains(QStringLiteral("ВВЕРХ"));
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Bottom);
            check(dimensions->accessibleDescription().contains(up ? QStringLiteral("ВНИЗ")
                                                                  : QStringLiteral("ВВЕРХ")),
                  "Reverse flat side kept the old bend direction");
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
            const auto returnedLabels = dimensions->accessibleDescription();
            check(measuredLabels(returnedLabels) == measuredLabels(topLabels),
                  "Returning to the flat side changed the selected measurement");
            viewer->highlightFaces({}, c.session().revision() + 1);
            check(dimensions->accessibleDescription() == returnedLabels,
                  "Stale selection changed flat labels");
            sendClick(QPoint(20, canvas->height() / 2));
            check(picked == -1 && !bends->currentIndex().isValid() &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · L ")),
                  "Empty flat click retained selected dimensions");
            // Source-list changes must update this same flat snapshot.
            bends->setCurrentIndex(bends->model()->index(0, 0));
            check(dimensions->accessibleDescription().contains(QStringLiteral("B01 ·")) &&
                      c.flatPattern() == first && !c.hasWork(),
                  "List selection did not refresh the flat bend or triggered a calculation");
            if (const auto measures = first->info().measurements;
                measures && !measures->openings.empty()) {
                const auto image = viewer->captureImage();
                int minX = image.width(), maxX = 0, minY = image.height(), maxY = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.red() > 50 && color.green() > 110 &&
                            color.green() > color.red() * 1.35 && color.blue() > color.green()) {
                            minX = std::min(minX, x);
                            maxX = std::max(maxX, x);
                            minY = std::min(minY, y);
                            maxY = std::max(maxY, y);
                        }
                    }
                check(maxX > minX && maxY > minY, "Cannot locate rendered flat stock");
                const auto &opening = measures->openings.front();
                const auto &bounds = first->info().bounds;
                const QPoint position(
                    qRound(minX + opening.center.x * (maxX - minX) / bounds.maximum.x),
                    qRound(maxY - opening.center.y * (maxY - minY) / bounds.maximum.y));
                const auto beforeClick = c.session().editRevision();
                std::cout << "Opening center " << opening.center.x << ',' << opening.center.y
                          << " pixel " << position.x() << ',' << position.y() << " body " << minX
                          << ',' << minY << ':' << maxX << ',' << maxY << '\n';
                sendClick(position / canvas->devicePixelRatioF());
                check(dimensions->accessibleDescription().contains(
                          opening.circular ? QStringLiteral("Отверстие") : QStringLiteral("Вырез")),
                      "Flat opening click did not reveal measured dimensions");
                check(dimensions->accessibleDescription().contains(QStringLiteral("Развёртка")) &&
                          dimensions->accessibleDescription().contains(QStringLiteral("Ширина")) &&
                          c.session().editRevision() == beforeClick && !c.hasWork(),
                      "Opening selection removed permanent dimensions or changed CAD/project");
                if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT");
                    !path.isEmpty())
                    viewer->captureImage(true).save(path + "-opening.png");
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(canvas, &escape);
                check(!dimensions->accessibleDescription().contains(QStringLiteral("Отверстие")) &&
                          !dimensions->accessibleDescription().contains(QStringLiteral("Вырез")) &&
                          dimensions->accessibleDescription().contains(QStringLiteral("Развёртка")),
                      "Escape did not preserve the permanent dimension layer");
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
            viewer->highlightFaces({}, revision);
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty()) {
                viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
                viewer->captureImage(true).save(path + "-source.png");
            }
            if (const auto nativeDoc =
                    std::dynamic_pointer_cast<const cad::CadDocument>(c.session().document())) {
                const auto data = cad::OcctDocumentAccess::measurements(*nativeDoc);
                if (data && !data->openings.empty()) {
                    viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
                    const auto image = viewer->captureImage();
                    int minX = image.width(), maxX = 0, minY = image.height(), maxY = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto color = image.pixelColor(x, y);
                            if (color.red() > 50 && color.green() > 110 &&
                                color.green() > color.red() * 1.35 &&
                                color.blue() > color.green()) {
                                minX = std::min(minX, x);
                                maxX = std::max(maxX, x);
                                minY = std::min(minY, y);
                                maxY = std::max(maxY, y);
                            }
                        }
                    const auto &bounds = nativeDoc->metadata().bounds;
                    const auto project = [&](application::PointMm p) {
                        return QPoint(qRound(minX + (p.x - bounds.minimum.x) * (maxX - minX) /
                                                        (bounds.maximum.x - bounds.minimum.x)),
                                      qRound(maxY - (p.y - bounds.minimum.y) * (maxY - minY) /
                                                        (bounds.maximum.y - bounds.minimum.y))) /
                               canvas->devicePixelRatioF();
                    };
                    const auto measurementEdit = c.session().editRevision();
                    int drawingSelections = 0;
                    const auto connection =
                        QObject::connect(viewer, &viewer::ViewerWidget::selectionChanged, &window,
                                         [&](quint64, int) { ++drawingSelections; });
                    const auto &opening = data->openings.front();
                    const auto rim =
                        std::find_if(data->edges.begin(), data->edges.end(), [&](const auto &edge) {
                            return std::find(opening.edges.begin(), opening.edges.end(), edge.id) !=
                                   opening.edges.end();
                        });
                    check(rim != data->edges.end(), "Source hole has no prepared rim");
                    sendClick(project(rim->first));
                    check(
                        dimensions->accessibleDescription().contains(QStringLiteral("Отверстие")) &&
                            dimensions->accessibleDescription().contains(QStringLiteral("X ")),
                        "Source hole selection lost permanent dimensions or did not use prepared "
                        "hole data");
                    const auto face = std::max_element(
                        data->faces.begin(), data->faces.end(),
                        [](const auto &a, const auto &b) { return a.areaMm2 < b.areaMm2; });
                    if (face != data->faces.end()) {
                        sendClick(project(face->anchor));
                        check(dimensions->accessibleDescription().contains(
                                  QStringLiteral("Плоская грань")),
                              "Source plane selection did not reveal its measured area");
                    }
                    const auto edge = std::max_element(
                        data->edges.begin(), data->edges.end(),
                        [](const auto &a, const auto &b) { return a.lengthMm < b.lengthMm; });
                    if (edge != data->edges.end() && edge->straight) {
                        sendClick(project({(edge->first.x + edge->second.x) / 2,
                                           (edge->first.y + edge->second.y) / 2,
                                           (edge->first.z + edge->second.z) / 2}));
                        check(dimensions->accessibleDescription().contains(QStringLiteral("Ребро")),
                              "Source edge selection did not reveal its actual length");
                    }
                    check(drawingSelections == 0 && c.session().editRevision() == measurementEdit &&
                              !c.hasWork(),
                          "Measurement picking changed drawing/project inputs or started CAD work");
                    QObject::disconnect(connection);
                }
            }
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
            bend.remove("workpieceOffsetMm");
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
