#include "bending/ui/MainWindow.h"
#include "BendMessagePanel.h"
#include "DrawingDialog.h"
#include "PartInputDialog.h"
#include "ProcessStepButton.h"
#include "ToolSearchDialog.h"
#include "ToolSelectionWidget.h"
#include "ToolWorkspace.h"
#include "UpdateDialog.h"
#include "bending/core/ToolGeometry.h"
#include "bending/ui/Preferences.h"
#include "bending/update/UpdateClient.h"

#include "bending/ui/ImportController.h"
#include "bending/viewer/ViewerWidget.h"

#include "RoutePanel.h"
#include "ToolSegmentColorPreferences.h"
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace bending::ui {
namespace {
using namespace application;
QIcon workpieceOrientationIcon(bool reversed) {
    QIcon icon;
    for (int ratio : {1, 2}) {
        QPixmap pixmap(48 * ratio, 32 * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#087F83"), 1.5));
        painter.drawArc(QRectF(6, 2, 36, 12), 20 * 16, 285 * 16);
        painter.setBrush(QColor("#087F83"));
        QPolygonF arrow{QPointF(39, 8), QPointF(43, 4), QPointF(43, 10)};
        painter.drawPolygon(arrow);
        if (reversed) {
            painter.translate(48, 0);
            painter.scale(-1, 1);
        }
        QPainterPath sheet;
        sheet.moveTo(9, 17);
        sheet.lineTo(13, 17);
        sheet.lineTo(13, 26);
        sheet.lineTo(38, 26);
        sheet.lineTo(38, 30);
        sheet.lineTo(9, 30);
        sheet.closeSubpath();
        painter.setPen(QPen(QColor("#075F65"), 1));
        painter.setBrush(QColor(reversed ? "#087F83" : "#718894"));
        painter.drawPath(sheet);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
QIcon punchOrientationIcon(bool reversed) {
    QPainterPath profile;
    profile.moveTo(8, 3);
    profile.lineTo(15, 3);
    profile.lineTo(15, 8);
    profile.lineTo(24, 8);
    profile.lineTo(24, 13);
    profile.lineTo(19, 15);
    profile.lineTo(17, 23);
    profile.lineTo(16, 29);
    profile.lineTo(13, 27);
    profile.lineTo(10, 21);
    profile.quadTo(6, 15, 10, 13);
    profile.lineTo(4, 12);
    profile.lineTo(4, 8);
    profile.lineTo(8, 8);
    profile.closeSubpath();
    QIcon icon;
    for (int ratio : {1, 2}) {
        QPixmap pixmap(48 * ratio, 32 * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#94A3AE"), 1, Qt::DashDotLine));
        painter.drawLine(QPointF(24, 1), QPointF(24, 31));
        for (bool mirrored : {false, true}) {
            painter.save();
            if (mirrored) {
                painter.translate(48, 0);
                painter.scale(-1, 1);
            }
            painter.scale(0.75, 1);
            const bool active = mirrored == reversed;
            painter.setPen(QPen(QColor(active ? "#075F65" : "#8595A2"), 1));
            painter.setBrush(QColor(active ? "#087F83" : "#B9C4CD"));
            painter.drawPath(profile);
            painter.restore();
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString displayPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return text(path.native());
#endif
}
std::filesystem::path nativePath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}
QString fileDropPath(const QMimeData *mime, bool inspectFile) {
    if (!mime || !mime->hasUrls())
        return {};
    const auto urls = mime->urls();
    if (urls.size() != 1 || !urls.front().isLocalFile())
        return {};
    const QFileInfo file(urls.front().toLocalFile());
    const auto suffix = file.suffix().toLower();
    if (suffix != QStringLiteral("step") && suffix != QStringLiteral("stp") &&
        suffix != QStringLiteral("bend"))
        return {};
    return !inspectFile || file.isFile() ? file.filePath() : QString{};
}
QString number(double value) {
    return QLocale::system().toString(value, 'g', 8);
}
QString surfaceName(SurfaceKind kind) {
    switch (kind) {
    case SurfaceKind::Plane:
        return QStringLiteral("Плоскость");
    case SurfaceKind::Cylinder:
        return QStringLiteral("Цилиндр");
    case SurfaceKind::Cone:
        return QStringLiteral("Конус");
    case SurfaceKind::Sphere:
        return QStringLiteral("Сфера");
    case SurfaceKind::Torus:
        return QStringLiteral("Тор");
    case SurfaceKind::Bezier:
        return QStringLiteral("Поверхность Безье");
    case SurfaceKind::BSpline:
        return QStringLiteral("B-сплайн");
    case SurfaceKind::Other:
        return QStringLiteral("Другая поверхность");
    }
    return {};
}
QString phaseName(ImportPhase phase) {
    switch (phase) {
    case ImportPhase::Waiting:
        return QStringLiteral("Ожидание загрузки");
    case ImportPhase::Reading:
        return QStringLiteral("Чтение STEP");
    case ImportPhase::Transferring:
        return QStringLiteral("Построение CAD-геометрии");
    case ImportPhase::Validating:
        return QStringLiteral("Проверка геометрии");
    case ImportPhase::Analyzing:
        return QStringLiteral("Анализ толщины и сгибов");
    case ImportPhase::Preparing:
        return QStringLiteral("Подготовка 3D-модели");
    }
    return {};
}
QString errorText(ImportErrorCode code) {
    switch (code) {
    case ImportErrorCode::UnsupportedFormat:
        return QStringLiteral("Выберите STEP (.step, .stp) или проект (.bend). Родные форматы CAD "
                              "пока не поддерживаются.");
    case ImportErrorCode::FileNotFound:
        return QStringLiteral("Файл не найден.");
    case ImportErrorCode::ReadFailed:
        return QStringLiteral("Не удалось прочитать файл. Проверьте путь и права доступа.");
    case ImportErrorCode::EmptyFile:
        return QStringLiteral("Выбранный файл пуст.");
    case ImportErrorCode::InvalidStep:
        return QStringLiteral("Файл содержит ошибки STEP и не может быть открыт.");
    case ImportErrorCode::NoGeometry:
        return QStringLiteral("В файле не найдена геометрия для просмотра.");
    case ImportErrorCode::UnsupportedUnits:
        return QStringLiteral("Единицы длины в STEP отсутствуют или заданы некорректно.");
    case ImportErrorCode::UnsupportedComposition:
        return QStringLiteral("Поддерживается отдельная деталь из одного объёмного тела. Сборки, "
                              "несколько тел и свободные поверхности пока не поддерживаются.");
    case ImportErrorCode::TransferFailed:
        return QStringLiteral("Не удалось полностью перенести геометрию STEP.");
    case ImportErrorCode::InvalidGeometry:
        return QStringLiteral("Геометрия детали некорректна.");
    case ImportErrorCode::KernelFailure:
        return QStringLiteral("Геометрическое ядро не смогло обработать файл.");
    case ImportErrorCode::Cancelled:
        return QStringLiteral("Загрузка отменена.");
    case ImportErrorCode::PreparationFailed:
        return QStringLiteral("Не удалось подготовить деталь для 3D-просмотра.");
    case ImportErrorCode::PresentationFailed:
        return QStringLiteral("Не удалось показать новую деталь. Предыдущая модель сохранена.");
    case ImportErrorCode::InvalidProject:
        return QStringLiteral(
            "Не удалось открыть проект: файл повреждён или параметры не согласованы с геометрией.");
    case ImportErrorCode::UnsupportedProjectVersion:
        return QStringLiteral("Эта версия формата проекта не поддерживается.");
    case ImportErrorCode::UnexpectedFailure:
        return QStringLiteral("Неожиданная ошибка загрузки.");
    }
    return {};
}
QString diagnosticsText(const std::vector<ImportDiagnostic> &diagnostics) {
    QStringList lines;
    for (const auto &entry : diagnostics) {
        lines.append(QStringLiteral("%1 · %2 · #%3: %4")
                         .arg(entry.stage == ImportStage::Read ? QStringLiteral("Чтение")
                                                               : QStringLiteral("Перенос"),
                              entry.severity == DiagnosticSeverity::Error
                                  ? QStringLiteral("Ошибка")
                                  : QStringLiteral("Предупреждение"))
                         .arg(entry.entityNumber)
                         .arg(text(entry.message)));
    }
    return lines.join(QLatin1Char('\n'));
}
QString reviewText(BendReview review) {
    switch (review) {
    case BendReview::Automatic:
        return QStringLiteral("Распознано автоматически");
    case BendReview::Pending:
        return QStringLiteral("Не проверен");
    case BendReview::Confirmed:
        return QStringLiteral("Подтверждён");
    case BendReview::Excluded:
        return QStringLiteral("Исключён");
    }
    return {};
}
} // namespace

class MainWindow::Impl {
  public:
    explicit Impl(MainWindow *parentWindow) : window(parentWindow) {}

    void refresh() {
        const auto &session = controller->session();
        const auto &document = session.document();
        if (!shownRevision || *shownRevision != session.revision()) {
            shownRevision = session.revision();
            selection->setText(QStringLiteral("Грань не выбрана. Нажмите ЛКМ на модели."));
            project->clear();
            if (document) {
                const auto &metadata = document->metadata();
                project->addItem(displayPath(metadata.sourcePath.filename()));
                project->setToolTip(displayPath(metadata.sourcePath).toHtmlEscaped());
                project->addItem(
                    QStringLiteral("Одно объёмное тело · %1 граней").arg(metadata.faceCount));

                QStringList units;
                for (const auto &unit : metadata.sourceLengthUnits) {
                    units.append(text(unit));
                }
                const auto &bounds = metadata.bounds;
                properties->setText(
                    QStringLiteral(
                        "Файл: %1\n\nГабариты: %2 × %3 × %4 мм\nОбъём: %5 мм³\nГраней: "
                        "%6\n\nЕдиницы файла: %7\nВ программе: мм\nДопуск BRep (макс.): %8 "
                        "мм\n\nИсходная CAD-геометрия.")
                        .arg(displayPath(metadata.sourcePath.filename()),
                             number(bounds.maximum.x - bounds.minimum.x),
                             number(bounds.maximum.y - bounds.minimum.y),
                             number(bounds.maximum.z - bounds.minimum.z),
                             number(metadata.volumeMm3))
                        .arg(metadata.faceCount)
                        .arg(units.join(QStringLiteral(", ")),
                             number(metadata.maximumToleranceMm)));
                window->setWindowTitle(displayPath(metadata.sourcePath.filename()) +
                                       QStringLiteral(" — ") + title);
            } else {
                project->addItem(QStringLiteral("Деталь не загружена"));
                project->setToolTip({});
                properties->setText(
                    QStringLiteral("Перетащите STEP/STP или проект .bend в окно, либо выберите "
                                   "Файл → Открыть…\n\nПоддерживается одно "
                                   "корректное объёмное тело.\nЕдиницы будут приведены к мм."));
                window->setWindowTitle(title);
            }
        }
        const bool newGeometry = shownBendRevision != session.revision();
        if (newGeometry) {
            wantFlat = false;
            displayedFlat.reset();
            shownBendRevision = session.revision();
            fillBends();
        }
        if (shownThicknessRevision != session.revision() ||
            shownManualThickness != session.manualThicknessMm()) {
            shownThicknessRevision = session.revision();
            shownManualThickness = session.manualThicknessMm();
            fillThicknessControls();
        }
        if (newGeometry) {
            if (document && !document->metadata().bendAnalysis.bends.empty())
                bendList->setCurrentIndex(bendModel->index(0, 0));
            viewingSource = true;
            viewingDraft = false;
            viewingRoute = false;
            setupAttempt.reset();
            pendingBendStage.reset();
            propertyTabs->setCurrentIndex(0);
        }
        promptPartInputs(newGeometry);
        const bool editable = document && !controller->hasWork();
        thicknessInput->setEnabled(editable);
        applyThickness->setEnabled(editable && thicknessInput->value() > 0.0);
        automaticThickness->setEnabled(editable && session.manualThicknessMm().has_value());
        pairs->setEnabled(editable && pairs->count() > 0);
        showPair->setEnabled(editable && viewer->isReady() && pairs->count() > 0);
        showCutouts->setEnabled(editable && viewer->isReady());
        saveAction->setEnabled(editable && writer != nullptr);
        saveAsAction->setEnabled(editable && writer != nullptr);
        if (document) {
            const auto &analysis = document->metadata().thickness;
            QString description;
            if (session.manualThicknessMm()) {
                description = QStringLiteral("Толщина: %1 мм · задана вручную\n")
                                  .arg(number(*session.manualThicknessMm()));
                const auto validation = validateThickness(*session.manualThicknessMm(), analysis);
                description +=
                    validation == ThicknessValidation::Verified
                        ? QStringLiteral("Подтверждена геометрией плоской детали.")
                    : validation == ThicknessValidation::LocalMatch
                        ? QStringLiteral("Совпадает с локальным расстоянием. Постоянство толщины "
                                         "всей детали не проверено.")
                        : QStringLiteral(
                              "Геометрией не подтверждена. Постоянство толщины не проверено.");
            } else if (analysis.status == ThicknessStatus::Detected && analysis.thicknessMm) {
                description = QStringLiteral("Толщина: %1 мм · определена по геометрии\nПлоская "
                                             "деталь постоянной толщины.")
                                  .arg(number(*analysis.thicknessMm));
            } else if (const auto suggestion = suggestThickness(analysis)) {
                description =
                    QStringLiteral("Предполагаемая толщина: %1 мм · по локальным "
                                   "измерениям\nПостоянство толщины всей детали не подтверждено.")
                        .arg(number(*suggestion));
            } else {
                description =
                    analysis.status == ThicknessStatus::LimitReached
                        ? QStringLiteral("Автоопределение: превышен лимит сложности.")
                    : analysis.status == ThicknessStatus::Failed
                        ? QStringLiteral(
                              "Автоопределение: геометрический анализ завершился ошибкой.")
                    : analysis.status == ThicknessStatus::Ambiguous
                        ? QStringLiteral("Толщина определена неоднозначно.")
                        : QStringLiteral("Согласованная толщина по стенкам и гибам не найдена.");
                description += QStringLiteral("\nМожно задать толщину вручную; это не подтверждает "
                                              "пригодность детали для гибки.");
            }
            thickness->setText(description);
            const auto name = session.projectPath().empty()
                                  ? displayPath(document->metadata().sourcePath.filename())
                                  : displayPath(session.projectPath().filename());
            window->setWindowTitle((session.modified() ? QStringLiteral("* ") : QString{}) + name +
                                   QStringLiteral(" — ") + title);
            projectState->setText(session.projectPath().empty()
                                      ? QStringLiteral("Проект ещё не сохранён")
                                      : QStringLiteral("Проект: %1%2")
                                            .arg(displayPath(session.projectPath().filename()),
                                                 session.modified()
                                                     ? QStringLiteral(" · есть изменения")
                                                     : QStringLiteral(" · сохранён")));
        } else {
            thickness->setText(QStringLiteral("Толщина: —"));
            projectState->clear();
        }
        bendList->setEnabled(document && !controller->hasWork());
        sheetSides->setVisible(document != nullptr);
        updateBendReviews();
        showSelectedBend(false);
        closeAction->setEnabled(document != nullptr || session.activeRequest().has_value());
        if (drawingAction)
            drawingAction->setEnabled(drawingDialog ||
                                      (document && drawingBuilder && !controller->hasWork()));
        const bool cancellable =
            session.activeRequest().has_value() || controller->isPreparingPreview() ||
            controller->isPreparingDrawing() || controller->isPreparingFlatPattern() ||
            controller->isPreparingRoute();
        cancelAction->setVisible(cancellable);
        cancelAction->setEnabled(cancellable);
        progress->setVisible(session.activeRequest().has_value() || controller->isSaving() ||
                             controller->isPreparingPreview() || controller->isPreparingDrawing() ||
                             controller->isPreparingFlatPattern() ||
                             controller->isPreparingRoute());
        if (controller->isPreparingRoute()) {
            window->statusBar()->showMessage(QStringLiteral("Подготовка 3D-порядка гибов…"));
        } else if (controller->isPreparingFlatPattern()) {
            window->statusBar()->showMessage(QStringLiteral("Подготовка теоретической развёртки…"));
        } else if (controller->isPreparingDrawing()) {
            window->statusBar()->showMessage(
                QStringLiteral("Подготовка проекций и размеров листа…"));
        } else if (controller->isPreparingPreview()) {
            window->statusBar()->showMessage(
                QStringLiteral("Восстановление заготовки и подготовка состояний сгиба…"));
        } else if (controller->isSaving()) {
            window->statusBar()->showMessage(QStringLiteral("Сохранение проекта…"));
        } else if (session.activeRequest()) {
            window->statusBar()->showMessage(QStringLiteral("%1 · %2").arg(
                phaseName(session.phase()),
                displayPath(session.activeRequest()->source.filename())));
        } else if (session.error()) {
            window->statusBar()->showMessage(errorText(session.error()->code));
        } else if (controller->hasWork()) {
            window->statusBar()->showMessage(
                QStringLiteral("Отмена запрошена · ожидаем завершения текущего вызова CAD"));
        } else if (!viewer->errorString().isEmpty()) {
            window->statusBar()->showMessage(
                QStringLiteral("3D-просмотр недоступен: %1").arg(viewer->errorString()));
        } else if (controller->bendPreview()) {
            window->statusBar()->showMessage(QStringLiteral("Просмотр операции"));
        } else {
            window->statusBar()->showMessage(
                document ? QStringLiteral("Деталь загружена")
                         : QStringLiteral("Откройте STEP для начала работы · Ctrl+O"));
        }
        const bool hasFailure = session.error().has_value() || !controller->saveError().isEmpty();
        error->setVisible(hasFailure);
        if (hasFailure && !showingFailure) {
            propertyTabs->setCurrentIndex(0);
        }
        showingFailure = hasFailure;
        QString detail;
        if (!controller->saveError().isEmpty()) {
            error->setText(
                QStringLiteral("Не удалось сохранить проект. Изменения остались в памяти."));
            detail = controller->saveError();
        } else if (session.error()) {
            error->setText(errorText(session.error()->code));
            detail = text(session.error()->detail) + QLatin1Char('\n') +
                     diagnosticsText(session.error()->diagnostics);
        } else if (document) {
            detail = diagnosticsText(document->metadata().diagnostics);
        }
        diagnostics->setPlainText(detail);
        diagnostics->setVisible(!detail.isEmpty());
        refreshPreview();
        refreshFlat();
        if (pendingBendStage && !controller->hasWork()) {
            const auto revision = *pendingBendStage;
            pendingBendStage.reset();
            QTimer::singleShot(0, window, [this, revision] {
                if (controller->session().revision() == revision &&
                    propertyTabs->currentIndex() == 2)
                    enterBendingStage();
            });
        }
    }

    std::optional<std::size_t> toolContextIndex() const {
        const auto &session = controller->session();
        for (const auto index : session.orderedBends())
            if ((toolContextId.empty() && included(session.bend(index)->review)) ||
                session.bend(index)->id == toolContextId)
                return index;
        return std::nullopt;
    }
    bool setToolContext(std::optional<std::size_t> index) {
        const auto &session = controller->session();
        const auto *entry = index ? session.bend(*index) : nullptr;
        const auto id = entry ? entry->id : std::string{};
        if (id != toolContextId && toolDraftChanged && !commitStageTools())
            return false;
        toolContextId = id;
        if (const auto selected = toolContextIndex())
            bendList->setCurrentIndex(bendModel->index(static_cast<int>(*selected), 0));
        refreshPreview();
        return true;
    }
    bool assignSelectedTool() {
        return commitStageTools();
    }
    void editRouteTool(std::size_t index) {
        const auto order = controller->session().orderedBends();
        const auto found = std::find(order.begin(), order.end(), index);
        if (found != order.end())
            routePanel->select(static_cast<int>(found - order.begin()) + 1, false);
        if (!setToolContext(index))
            return;
        propertyTabs->setCurrentIndex(3);
        refreshPreview();
    }

    bool requireRealTool(const std::optional<core::ToolAssignment> &assignment, std::size_t index) {
        const auto issue = ToolSelectionWidget::assignmentIssue(assignment);
        if (issue.isEmpty())
            return true;
        stageSetupIssue = QStringLiteral("B%1: %2").arg(index + 1).arg(issue);
        stageToolIssueIndex = index;
        refreshPreview();
        return false;
    }
    void prepareRoute(bool automatic, bool withTools, bool expanded = false,
                      std::vector<application::RouteToolVariant> variants = {}) {
        const auto &session = controller->session();
        if (!session.document() || controller->hasWork())
            return;
        stageSetupIssue.clear();
        stageToolIssueIndex.reset();
        application::BendRouteRequest request;
        request.automatic = automatic;
        request.allowWorkpieceRotation = automatic;
        request.withTools = withTools;
        request.toolVariants = std::move(variants);
        const bool checkedTooling = !request.toolVariants.empty();
        request.allowPartialEndSupport = withTools;
        if (withTools)
            request.maximumEndSupportLossMm = toolSelection->maximumEndSupportLossMm();
        if (expanded || checkedTooling) {
            request.maximumChecks = 512;
            request.maximumTimeMs = 120000;
        }
        const bool perBend = withTools && (!checkedTooling || !toolContextId.empty());
        if (!checkedTooling && withTools && !commitStageTools()) {
            propertyTabs->setCurrentIndex(3);
            return;
        }
        auto tools = toolSelection->parameters();
        auto assignment = toolSelection->assignment();
        if (checkedTooling && !perBend) {
            assignment = request.toolVariants.front().assignment;
            tools = core::toolParameters(*assignment);
        }
        routePanel->setWithTools(withTools);
        if (!checkedTooling)
            propertyTabs->setCurrentIndex(2);
        if (withTools && !perBend && !tools) {
            refreshPreview();
            return;
        }
        for (auto index : session.orderedBends()) {
            const auto *entry = session.bend(index);
            const auto target = session.bendTarget(index);
            if (!entry || !application::included(entry->review) || !target) {
                refreshPreview();
                return;
            }
            const bool replacing =
                checkedTooling && (!request.toolVariants.front().bendIndex ||
                                   request.toolVariants.front().bendIndex == index);
            const auto operationAssignment =
                replacing ? std::optional{request.toolVariants.front().assignment}
                : perBend ? entry->catalogTools
                          : assignment;
            const auto operationTools = replacing ? core::toolParameters(*operationAssignment)
                                        : perBend ? entry->tools
                                                  : tools;
            if (perBend && !operationTools) {
                editRouteTool(index);
                return;
            }
            if (withTools && !requireRealTool(operationAssignment, index))
                return;
            request.operations.push_back(
                {entry->id, index, *target, !withTools ? std::nullopt : operationTools,
                 !withTools ? std::nullopt : operationAssignment, entry->workpieceReversed});
        }
        if (!checkedTooling) {
            beginPreviewReplacement();
            stopPlayback();
            viewingDraft = false;
            draftAutoPlay = false;
            viewingRoute = true;
            viewingSource = false;
            wantFlat = false;
        } else {
            checkedToolVariants = request.toolVariants;
            checkedToolJob = true;
            checkedToolCancelled = checkedToolManualOverride = false;
            checkedToolDocument = session.revision();
            checkedToolEdit = session.editRevision();
            checkedToolDialog->begin();
            toolSelection->setCheckedSearchState(true,
                                                 QStringLiteral("Проверяем инструмент и порядок…"));
        }
        lastRouteAutomatic = automatic;
        routePanel->setExpandedSearch(expanded);
        if (controller->prepareBendRoute(std::move(request), previewBuilder)) {
            if (withTools && !checkedTooling) {
                toolDraftChanged = false;
                toolSelection->rememberChoice();
            }
            if (!checkedTooling)
                showPlaybackPanel();
        } else if (checkedTooling) {
            checkedToolJob = false;
            checkedToolVariants.clear();
            checkedToolDialog->finish(false);
            toolSelection->setCheckedSearchState(
                false, QStringLiteral(
                           "Полная проверка недоступна. Используйте быстрый или ручной выбор."));
        }
    }
    void cancelCheckedToolSelection(bool editing = false) {
        if (!checkedToolJob && checkedToolVariants.empty())
            return;
        checkedToolVariants.clear();
        checkedToolCancelled = true;
        checkedToolManualOverride |= editing;
        if (checkedToolDialog)
            checkedToolDialog->hide();
        if (checkedToolJob && controller->hasWork())
            controller->cancel();
        else
            toolSelection->setCheckedSearchState(false, editing ? QString{}
                                                                : QStringLiteral("Подбор отменён"));
    }
    void prepareCheckedToolSelection() {
        if (checkedToolJob && controller->hasWork()) {
            cancelCheckedToolSelection();
            return;
        }
        const auto &session = controller->session();
        if (controller->hasWork() || !session.document())
            return;
        const bool perBend = !toolContextId.empty();
        const auto row = bendList->currentIndex().row();
        if (perBend && row < 0)
            return;
        std::optional<std::size_t> target;
        if (perBend)
            target = static_cast<std::size_t>(row);
        if (!checkedToolVariants.empty() && checkedToolDocument == session.revision() &&
            checkedToolEdit == session.editRevision() &&
            checkedToolVariants.front().bendIndex == target) {
            prepareRoute(true, true, true, checkedToolVariants);
            return;
        }
        std::vector<application::RouteToolVariant> variants;
        for (auto &a : toolSelection->checkedCandidates())
            variants.push_back({std::move(a), target});
        if (variants.empty()) {
            toolSelection->setCheckedSearchState(
                false, QStringLiteral("Нет подходящих DXF-профилей. Доступен ручной выбор."));
            return;
        }
        prepareRoute(true, true, true, std::move(variants));
    }
    void suggestInitialTools() {
        const auto &session = controller->session();
        if (!session.document() || controller->hasWork() ||
            initialToolSuggestion == session.revision())
            return;
        initialToolSuggestion = session.revision();
        const auto order = session.orderedBends();
        if (toolDraftChanged || std::any_of(order.begin(), order.end(), [&](auto index) {
                return session.bend(index)->tools.has_value();
            }))
            return;
        refresh();
        toolSelection->suggestForNewPart();
    }

    void checkTools() {
        if (controller->hasWork())
            return;
        const auto &document = controller->session().document();
        if (toolMode->currentIndex() == 0 && document &&
            document->metadata().bendAnalysis.bends.size() > 1) {
            prepareRoute(lastRouteAutomatic, true);
            return;
        }
        viewingRoute = false;
        const auto row = bendList->currentIndex().row();
        const auto tools = toolSelection->parameters();
        if (!tools) {
            propertyTabs->setCurrentIndex(3);
            refreshPreview();
            return;
        }
        if (!applyTools->isEnabled() || row < 0) {
            propertyTabs->setCurrentIndex(2);
            refreshPreview();
            return;
        }
        beginPreviewReplacement();
        if (!commitStageTools())
            return;
        viewingSource = false;
        propertyTabs->setCurrentIndex(2);
        if (controller->prepareBendPreview(static_cast<std::size_t>(row), false, previewBuilder,
                                           toolMode->currentIndex() == 0
                                               ? BendPreviewMode::ToolMotion
                                               : BendPreviewMode::ToolSetup,
                                           toolSelection->maximumEndSupportLossMm()))
            showPlaybackPanel();
    }

    void showSource() {
        stopPlayback();
        previewReplacement.reset();
        draftAutoPlay = false;
        viewingSource = true;
        wantFlat = false;
        if (displayedPreview || displayedFlat) {
            displayedPreview.reset();
            displayedFlat.reset();
            previewDisplayError = viewer->endBendPreview();
        }
    }

    bool commitStageTools() {
        const auto &session = controller->session();
        const auto selected = toolContextIndex();
        const auto tools = toolSelection->parameters();
        const auto assignment = toolSelection->assignment();
        if (!selected || !tools || controller->hasWork())
            return false;
        const auto *current = session.bend(*selected);
        const bool upgrade = current && current->catalogTools &&
                             !current->catalogTools->useDrawingGeometry && assignment &&
                             core::hasCompleteDrawingToolGeometry(*assignment);
        if (!toolDraftChanged && current && current->tools && !upgrade)
            return true;
        std::vector<application::BendToolUpdate> toolUpdates;
        for (const auto index : session.orderedBends()) {
            const auto *entry = session.bend(index);
            if (!entry || !included(entry->review) ||
                (!toolContextId.empty() && entry->id != toolContextId))
                continue;
            auto actual = assignment;
            // The global editor patches only fields the user changed. Local setups keep
            // their other tool, sections, orientation and independent workpiece position.
            if (toolContextId.empty() && assignment && shownAssignment && entry->catalogTools) {
                actual = entry->catalogTools;
                const auto copyChanged = [&](auto member) {
                    if ((*assignment).*member != (*shownAssignment).*member)
                        (*actual).*member = (*assignment).*member;
                };
                copyChanged(&core::ToolAssignment::punch);
                copyChanged(&core::ToolAssignment::die);
                copyChanged(&core::ToolAssignment::groove);
                if (actual->punch == assignment->punch)
                    copyChanged(&core::ToolAssignment::punchAssembly);
                if (actual->die == assignment->die)
                    copyChanged(&core::ToolAssignment::dieAssembly);
                copyChanged(&core::ToolAssignment::clearanceMm);
                copyChanged(&core::ToolAssignment::machineName);
                copyChanged(&core::ToolAssignment::useDrawingGeometry);
                copyChanged(&core::ToolAssignment::machineModel);
                copyChanged(&core::ToolAssignment::punchReversed);
                copyChanged(&core::ToolAssignment::backgauge);
                // A different profile cannot retain sections belonging to the old profile.
                if (toolDraftPunchSelected || assignment->punch != shownAssignment->punch) {
                    actual->punch = assignment->punch;
                    actual->punchAssembly = assignment->punchAssembly;
                    actual->punchReversed = assignment->punchReversed;
                    actual->useDrawingGeometry = assignment->useDrawingGeometry;
                }
                if (toolDraftDieSelected || assignment->die != shownAssignment->die) {
                    actual->die = assignment->die;
                    actual->dieAssembly = assignment->dieAssembly;
                    actual->groove = assignment->groove;
                    actual->useDrawingGeometry = assignment->useDrawingGeometry;
                }
            }
            const auto parameters = actual ? core::toolParameters(*actual) : tools;
            if (!parameters)
                return false;
            toolUpdates.push_back({index, parameters, std::move(actual)});
        }
        if (toolUpdates.empty())
            return false;
        const bool wasDirty = toolDraftChanged;
        toolDraftChanged = false;
        if (!controller->setBendToolAssignments(toolUpdates)) {
            toolDraftChanged = wasDirty;
            return false;
        }
        toolDraftPunchSelected = toolDraftDieSelected = false;
        toolSelection->rememberChoice();
        return true;
    }

    void enterBendingStage(bool applySelection = false) {
        stopPlayback();
        stageSetupIssue.clear();
        stageToolIssueIndex.reset();
        if (controller->hasWork()) {
            if (!controller->isPreparingPreview() && !controller->isPreparingRoute() &&
                !controller->isPreparingDraft() && !controller->session().activeRequest())
                pendingBendStage = controller->session().revision();
            viewingSource = false;
            wantFlat = false;
            propertyTabs->setCurrentIndex(2);
            refreshPreview();
            return;
        }
        pendingBendStage.reset();
        if (!controller->session().document()) {
            propertyTabs->setCurrentIndex(2);
            refreshPreview();
            return;
        }
        const bool committed = commitStageTools();
        if (committed && applySelection) {
            if (const auto previous = scenePreview(); previous && !previous->toolSetup()) {
                beginPreviewReplacement();
                controller->discardBendRoute();
                controller->discardBendPreview();
                viewingDraft = false;
                viewingRoute = false;
                setupAttempt.reset();
            }
        }
        if (!committed)
            stageSetupIssue = toolSelection->geometryIssue().isEmpty()
                                  ? toolSelection->openingIssue()
                                  : toolSelection->geometryIssue();
        if (committed)
            for (auto index : controller->session().orderedBends()) {
                const auto *entry = controller->session().bend(index);
                if (entry && included(entry->review) && !entry->tools) {
                    stageSetupIssue =
                        QStringLiteral("Для гиба B%1 не выбран инструмент.").arg(index + 1);
                    stageToolIssueIndex = index;
                    break;
                }
            }
        if (controller->bendRoute()) {
            viewingRoute = true;
            viewingDraft = false;
        } else if (controller->routeDraft()) {
            viewingDraft = true;
            viewingRoute = false;
        }
        viewingSource = false;
        wantFlat = false;
        propertyTabs->setCurrentIndex(2);
        refreshPreview();
        if (scenePreview() || !stageSetupIssue.isEmpty() || !previewBuilder)
            return;
        const auto key =
            std::pair{controller->session().revision(), controller->session().editRevision()};
        if (setupAttempt == key)
            return;
        setupAttempt = key;
        if (controller->session().document()->metadata().bendAnalysis.bends.size() > 1) {
            routePanel->setWithTools(true);
            routePanel->select(routePanel->selected());
        } else if (preparePreview->isEnabled()) {
            // Keep the existing complete single-bend cycle and its collision/return checks.
            checkTools();
        }
    }

    void editPartInputs() {
        if (partInputDialog) {
            partInputDialog->raise();
            partInputDialog->activateWindow();
            return;
        }
        if (!controller->session().document() || controller->hasWork())
            return;
        partInputDialog = new PartInputDialog(*controller, window);
        auto *dialog = partInputDialog.data();
        QObject::connect(dialog, &QDialog::finished, window, [this, dialog] {
            if (partInputDialog == dialog)
                partInputDialog.clear();
        });
        dialog->show();
    }
    void promptPartInputs(bool newGeometry) {
        // A new request invalidates queued prompts, including a request later cancelled/failed.
        if (controller->session().activeRequest())
            ++inputPromptSerial;
        if (!newGeometry || !automaticInputPrompt || !controller->session().document())
            return;
        const auto &session = controller->session();
        if (!session.projectPath().empty() && session.hasPartInputs())
            return;
        const auto serial = ++inputPromptSerial;
        const auto revision = session.revision();
        const auto edit = session.editRevision();
        QTimer::singleShot(0, window, [this, serial, revision, edit] {
            const auto &current = controller->session();
            if (automaticInputPrompt && serial == inputPromptSerial &&
                current.revision() == revision && current.editRevision() == edit &&
                !controller->hasWork() && !current.activeRequest())
                editPartInputs();
        });
    }
    void showFlat() {
        if (controller->hasWork() || !flatBuilder || !controller->session().document())
            return;
        if (!controller->session().hasPartInputs()) {
            editPartInputs();
            return;
        }
        stopPlayback();
        previewReplacement.reset();
        viewingSource = true;
        wantFlat = true;
        if (displayedPreview) {
            displayedPreview.reset();
            (void)viewer->endBendPreview();
        }
        controller->prepareFlatPattern(flatBuilder);
    }
    void refreshFlat() {
        const auto &s = controller->session();
        const auto settings = s.unfoldSettings();
        const bool editable = s.document() && !controller->hasWork();
        partInputs->setText(partInputsSummary(s));
        editInputs->setEnabled(editable);
        flatAction->setEnabled(editable && flatBuilder);
        flatAction->setText(controller->isPreparingFlatPattern()
                                ? QStringLiteral("Готовим развёртку…")
                            : controller->flatPattern() ? QStringLiteral("Показать развёртку")
                            : !s.hasPartInputs() ? QStringLiteral("Развёртка: исходные данные…")
                                                 : QStringLiteral("Подготовить развёртку"));
        for (auto *button : {flatButton, checkFlatButton})
            if (button) {
                button->setText(flatAction->text());
                button->setEnabled(flatAction->isEnabled());
            }
        const auto flat = controller->flatPattern();
        if (displayedFlat && displayedFlat != flat) {
            (void)viewer->endBendPreview();
            displayedFlat.reset();
        }
        QString status;
        if (controller->isPreparingFlatPattern())
            status = QStringLiteral("Раскладываем стенки и проверяем полный контур…");
        else if (const auto flatError = controller->flatPatternError()) {
            status = flatError->code == FlatPatternErrorCode::ThicknessConflict
                         ? QStringLiteral("Толщина отличается от CAD.")
                     : flatError->code == FlatPatternErrorCode::MissingMaterial
                         ? QStringLiteral("Выберите материал или свой K.")
                         : QStringLiteral("Развёртка этой формы пока не поддерживается.");
            flatStatus->setToolTip(text(flatError->detail));
        } else if (flat) {
            const auto &info = flat->info();
            status = QStringLiteral(
                         "Расчётная развёртка %1 × %2 мм · t %3 · гибов %4\nСвободная гибка · %5")
                         .arg(QLocale::system().toString(info.bounds.maximum.x, 'f', 2),
                              QLocale::system().toString(info.bounds.maximum.y, 'f', 2),
                              number(info.thicknessMm))
                         .arg(info.bends.size())
                         .arg(settings.manualK ? QStringLiteral("K задан вручную")
                                               : QStringLiteral("справочник Wiz Parts v1"));
            QString details =
                QStringLiteral("Теоретический расчёт без калибровки станка. Координаты линий гибов "
                               "— от края заготовки, не X упора.");
            if (info.cutoutsIgnored) {
                status +=
                    QStringLiteral("\nОтверстия показаны; их влияние на гибку не учитывается.");
                details += QStringLiteral(" Проверена отдельная копия без замкнутых вырезов. "
                                          "Форма вырезов в зоне гиба показана условно.");
            }
            for (const auto &b : info.bends)
                details += QStringLiteral("\nB%1: Ri %2 · K %3 · BA %4 мм · L %5 мм")
                               .arg(b.bendIndex + 1, 2, 10, QLatin1Char('0'))
                               .arg(number(b.radiusMm), number(b.k), number(b.allowanceMm),
                                    number(b.lengthMm()));
            flatStatus->setToolTip(details);
            if (checkFlatStatus)
                checkFlatStatus->setToolTip(details);
            if (wantFlat && displayedFlat != flat) {
                const auto displayError = viewer->presentFlatPattern(flat, s.revision());
                if (displayError.isEmpty()) {
                    displayedFlat = flat;
                    viewer->setDimensionsVisible(true);
                } else
                    status =
                        QStringLiteral("Развёртка рассчитана; ошибка показа: %1").arg(displayError);
            }
        } else
            status.clear();
        flatStatus->setText(status);
        flatStatus->setVisible(!status.isEmpty());
        if (checkFlatStatus)
            checkFlatStatus->setVisible(displayedFlat || controller->isPreparingFlatPattern() ||
                                        controller->flatPatternError().has_value());
        if (checkFlatStatus)
            checkFlatStatus->setText(status);
        if (checkFlatButton)
            checkFlatButton->setVisible(displayedFlat != nullptr);
        flatSource->setVisible(displayedFlat != nullptr);
        if (checkFlatSource)
            checkFlatSource->setVisible(displayedFlat != nullptr);
    }

    void refreshWorkflow() {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        const auto preview = scenePreview();
        const auto tools = toolSelection->parameters();
        const bool chosen =
            tools && (toolSelection->usesCatalog() || toolDraftChanged || (entry && entry->tools));
        partName->setText(document ? displayPath(document->metadata().sourcePath.filename())
                                   : QStringLiteral("Перетащите STEP/STP или .bend в окно"));
        partToolSummary->setText(
            chosen ? QStringLiteral("Инструмент: %1").arg(toolSelection->summary()) : QString{});
        partToolSummary->setVisible(document && chosen);
        partReadiness->setText(checkBlocker);
        partReadiness->setVisible(
            document && !checkBlocker.isEmpty() &&
            (!singleBendSupported || !matchingBendThickness || !entry || !included(entry->review)));
        projectState->setVisible(document &&
                                 (session.modified() || !session.projectPath().empty()));
        bendList->setVisible(bendModel->rowCount() > 1 || (!entry && bendModel->rowCount() > 0));
        editBend->setVisible(document && (row >= 0 || !session.bendBindings().unresolved.empty()));
        editBend->setEnabled(!controller->hasWork());
        changePartTools->setVisible(document && chosen);
        changePartTools->setEnabled(!controller->hasWork());
        partPrimary->setEnabled(!controller->hasWork());
        partPrimary->setText(document ? QStringLiteral("К инструменту →")
                                      : QStringLiteral("Открыть деталь…"));
        const auto bendCount = document ? document->metadata().bendAnalysis.bends.size() : 0;
        processSteps[0]->setStatus(document ? QStringLiteral("Найдено сгибов: %1").arg(bendCount)
                                            : QStringLiteral("Открыть STEP"));
        if (document && bendCount > 1 && !toolContextId.empty()) {
            std::size_t assigned = 0;
            for (const auto index : session.orderedBends())
                assigned += session.bend(index)->tools.has_value() ? 1 : 0;
            processSteps[1]->setStatus(
                QStringLiteral("По гибам: %1 из %2").arg(assigned).arg(bendCount));
        } else
            processSteps[1]->setStatus(chosen && document ? toolSelection->shortSummary()
                                                          : QStringLiteral("Выбрать пару"));
        const auto bendingStatus =
            controller->isPreparingDraft() ? QStringLiteral("Проверяем столкновения…")
            : viewingDraft && controller->routeDraft() ? QStringLiteral("Ручной порядок")
            : controller->isPreparingRoute() ? QStringLiteral("Подбираем порядок…")
            : controller->bendRoute() && controller->bendRoute()->partialEndSupport
                ? QStringLiteral("Просмотр: неполная опора")
            : scenePreview() && scenePreview()->partialEndSupport()
                ? QStringLiteral("Просмотр: неполная опора")
            : controller->bendRoute()
                ? QStringLiteral("Порядок: %1 шагов").arg(controller->bendRoute()->steps.size())
            : controller->isPreparingPreview() ? QStringLiteral("Проверяем…")
            : !document                        ? QStringLiteral("Нужна деталь")
            : !singleBendSupported ? QStringLiteral("Пока не поддерживается")
            : !matchingBendThickness || !entry || !included(entry->review)
                ? QStringLiteral("Нужно уточнение")
            : controller->bendPreviewError() || controller->bendRouteError() ||
                    !previewDisplayError.isEmpty()
                ? QStringLiteral("Есть проблема")
            : preview && preview->pressMotion() ? QStringLiteral("Проверено в модели")
            : preview ? QStringLiteral("Просмотр подготовлен")
            : !chosen ? QStringLiteral("Нужен инструмент")
                      : QStringLiteral("Нужна проверка");
        std::size_t activeBends = 0;
        for (auto i : session.orderedBends())
            activeBends += included(session.bend(i)->review);
        processSteps[2]->setStatus(
            document ? QStringLiteral("Гибов: %1 · %2").arg(activeBends).arg(bendingStatus)
                     : bendingStatus);
        const bool checking = propertyTabs->currentIndex() == 2 && !viewingSource;
        const bool transport =
            checking && (preview || previewReplacement || controller->isPreparingPreview() ||
                         controller->isPreparingDraft());
        playbackControls->setVisible(transport);
        operationOverlay->setVisible(checking && (transport || (document && bendCount > 1)));
        if (bendMessages)
            bendMessages->setActive(checking && document && !viewingSource && !wantFlat);
    }

    void showPlaybackPanel() {
        playbackControls->show();
        operationOverlay->show();
    }
    void stopPlayback() {
        playback->stop();
        playBend->setText(QStringLiteral("Воспроизвести"));
    }
    bool shortToolPlaybackBlocked() const {
        const auto draft = viewingDraft ? controller->routeDraft() : nullptr;
        return draft && draft->toolLengths && shortToolDiagnosticDraft.lock() != draft;
    }
    static bool collisionAt(const application::BendPreviewPtr &preview, std::size_t frame) {
        return preview &&
               (preview->collisionAtFrame(frame) || preview->machineCollisionAtFrame(frame));
    }
    void refreshPlaybackAvailability(const application::BendPreviewPtr &preview) {
        const bool lengthBlocked = shortToolPlaybackBlocked();
        if (lengthBlocked) {
            stopPlayback();
            draftAutoPlay = false;
        }
        const bool animated =
            preview && (preview->frameCount() > 1 || preview->unverifiedDraft() || viewingRoute);
        playBend->setEnabled(animated && !lengthBlocked && !viewingSource &&
                             !controller->hasWork() && previewDisplayError.isEmpty() &&
                             stageSetupIssue.isEmpty() &&
                             !collisionAt(preview, static_cast<std::size_t>(bendFrame->value())));
        playbackSpeedControls->setEnabled(playBend->isEnabled());
    }
    void setFrameText(const QString &text) {
        frameLabel->setText(text);
        const auto tooltip = shortToolPlaybackBlocked()
                                 ? QStringLiteral("Инструмент короче линии гиба. Подберите секции "
                                                  "или другой инструмент. Диагностический "
                                                  "просмотр доступен в подробностях проверки "
                                                  "выбранного гиба.")
                                 : text;
        bendFrame->setToolTip(tooltip);
        playBend->setToolTip(tooltip);
    }
    std::optional<std::size_t> selectedOperationIndex() const {
        if (viewingRoute && controller->bendRoute() && routePanel->selected() > 0)
            return controller->bendRoute()->steps[routePanel->operation()].bendIndex;
        if (controller->session().document() &&
            controller->session().document()->metadata().bendAnalysis.bends.size() > 1)
            return routePanel->selectedBend();
        const auto row = bendList->currentIndex().row();
        return row >= 0 ? std::optional{static_cast<std::size_t>(row)} : std::nullopt;
    }
    bool blankSetupSelected() const {
        return (viewingRoute || viewingDraft) && routePanel->selected() == 0;
    }
    std::optional<std::size_t> selectedPunchIndex() const {
        if (!blankSetupSelected())
            return selectedOperationIndex();
        if (const auto route = controller->bendRoute();
            viewingRoute && route && !route->steps.empty())
            return route->steps.front().bendIndex;
        const auto indices = controller->session().orderedBends();
        return indices.empty() ? std::nullopt : std::optional{indices.front()};
    }
    void beginPreviewReplacement(bool preserveFrame = false) {
        shortToolDiagnosticDraft.reset();
        if (!viewingSource && displayedPreview && !previewReplacement) {
            previewReplacement = PreviewReplacement{
                controller->session().document(), controller->session().revision(),
                displayedPreview, static_cast<std::size_t>(bendFrame->value()), preserveFrame};
            stopPlayback();
            setFrameText({});
        }
    }
    void setWorkpieceOrientation(bool reversed) {
        const auto index = selectedPunchIndex(); // Blank uses the first operation's installation.
        if (!index || controller->hasWork())
            return;
        const auto *entry = controller->session().bend(*index);
        if (!entry || entry->workpieceReversed == reversed)
            return;
        const bool multiple =
            controller->session().document()->metadata().bendAnalysis.bends.size() > 1;
        const int step = routePanel->selected();
        beginPreviewReplacement(true);
        stopPlayback();
        if (!controller->setBendWorkpieceOrientation(*index, reversed))
            return;
        if (multiple) {
            routePanel->select(step);
        } else {
            toolMode->setCurrentIndex(0);
            checkTools();
        }
    }
    void setPunchOrientation(bool reversed) {
        const auto index = selectedPunchIndex();
        if (!index || controller->hasWork() || (toolDraftChanged && !commitStageTools()))
            return;
        const auto &session = controller->session();
        const bool multi = session.document()->metadata().bendAnalysis.bends.size() > 1;
        const auto *entry = session.bend(*index);
        if (!entry || !entry->tools)
            return;
        auto tools = *entry->tools;
        auto assignment = entry->catalogTools;
        const bool all = multi && blankSetupSelected();
        if (!all && tools.punchReversed == reversed)
            return;
        beginPreviewReplacement(true);
        const int step = routePanel->selected();
        stopPlayback();
        if (all) {
            if (!controller->setAllBendPunchOrientations(reversed))
                return;
            routePanel->select(0);
            return;
        }
        tools.punchReversed = reversed;
        if (tools.machineModel == core::MachineModel::DemoPressV1)
            tools.machineModel = core::MachineModel::DemoPressV2;
        if (assignment) {
            assignment->punchReversed = reversed;
            assignment->machineModel = tools.machineModel;
        }
        if (!controller->setBendToolAssignments({{*index, tools, assignment}}))
            return;
        bendList->setCurrentIndex(bendModel->index(static_cast<int>(*index), 0));
        if (multi)
            routePanel->select(step);
        else {
            toolMode->setCurrentIndex(0);
            checkTools();
        }
    }
    void refreshCheckResult() {
        const auto &session = controller->session();
        const auto preview = scenePreview();
        const auto route = controller->bendRoute();
        const auto frame = static_cast<std::size_t>(bendFrame->value());
        const auto machineCollision =
            preview ? preview->machineCollisionAtFrame(frame) : std::nullopt;
        const bool collision = preview && (preview->collision() ||
                                           preview->collisionAtFrame(frame) || machineCollision);
        const bool partial = (preview && preview->partialEndSupport()) ||
                             (viewingRoute && route && route->partialEndSupport);
        const bool preparing = controller->isPreparingDraft() || controller->isPreparingRoute() ||
                               controller->isPreparingPreview();
        QString result;
        if (!stageSetupIssue.isEmpty() && !preparing)
            result = stageSetupIssue;
        else if (controller->isPreparingDraft())
            result = QStringLiteral("Проверяем столкновения выбранного шага…");
        else if (controller->isPreparingRoute())
            result = lastRouteAutomatic ? QStringLiteral("Подбираем порядок гибов…")
                                        : QStringLiteral("Проверяем ваш порядок…");
        else if (controller->isPreparingPreview())
            result = QStringLiteral("Проверяем гибку…");
        else if (!previewDisplayError.isEmpty())
            result = QStringLiteral("Не удалось показать положение.\n%1").arg(previewDisplayError);
        else if (viewingDraft && controller->routeDraftError()) {
            const auto draftError = *controller->routeDraftError();
            result =
                draftError.operationError == BendPreviewErrorCode::NarrowDieOpening
                    ? QStringLiteral("Раскрытие V слишком мало для толщины детали. "
                                     "Выберите другую матрицу; ориентир — около 8t.")
                    : QStringLiteral("Положение не проверено.\n%1").arg(text(draftError.detail));
        } else if (machineCollision && machineCollision->secondId == "floor")
            result = QStringLiteral("Упор в пол.\nИзмените порядок или установку детали.");
        else if (machineCollision)
            result =
                QStringLiteral("Столкновение: %1 — %2.\nКонфликтующие тела выделены красным.")
                    .arg(text(machineCollision->firstName), text(machineCollision->secondName));
        else if (preview && preview->unverifiedDraft()) {
            result = collision
                         ? QStringLiteral("Столкновение с инструментом.\nИзмените порядок или "
                                          "установку.")
                     : controller->routeDraft() && controller->routeDraft()->checkedToolPositions
                         ? QStringLiteral("В этом кадре пересечений с инструментом не найдено.\n"
                                          "Полный порядок, опора и отвод не проверены.")
                         : QStringLiteral("Инструмент не задан.\nСтолкновения не проверены.");
            if (controller->routeDraft() && !controller->routeDraft()->toolLengths &&
                !controller->routeDraft()->toolNotice.empty())
                result += QLatin1Char('\n') + text(controller->routeDraft()->toolNotice);
        } else if (controller->bendRouteError())
            result = routePanel->statusText();
        else if (collision)
            result = QStringLiteral("Обнаружено столкновение с инструментом.\n"
                                    "Положение выделено красным.");
        else if (controller->bendPreviewError())
            result = operationFailureSummary;
        else if (partial) {
            result = QStringLiteral("Просмотр с неполной опорой у торца.\n"
                                    "Пригодность к изготовлению не подтверждена.");
            if (preview && preview->partialEndSupport()) {
                const auto &s = *preview->partialEndSupport();
                result += QStringLiteral("\nНехватка у торцов: %1 / %2 мм; допуск %3 мм на каждый.")
                              .arg(s.supportedFromMm, 0, 'f', 2)
                              .arg(s.bendLengthMm - s.supportedToMm, 0, 'f', 2)
                              .arg(toolSelection->maximumEndSupportLossMm(), 0, 'f', 2);
            }
        } else if (preview && preview->pressMotion())
            result = (viewingRoute ? QStringLiteral("Порядок проверен в модели.")
                                   : QStringLiteral("Геометрическая проверка пройдена.")) +
                     (preview->machineModel() == core::MachineModel::None
                          ? QStringLiteral("\nГибка — по кадрам, отвод пуансона — непрерывно.\n"
                                           "Станок, держатели и перенос не проверены.")
                          : QString{});
        else if (preview && preview->toolSetup())
            result = QStringLiteral("Начальная установка проверена.\n"
                                    "Одно положение; движение не проверено.");
        else if (preview)
            result =
                QStringLiteral("Просмотр без инструмента.\nКонтакт и ход пресса не проверены.");
        else if (!checkBlocker.isEmpty())
            result = checkBlocker;
        else
            result = toolDraftChanged
                         ? QStringLiteral("Параметры изменены. Нужна новая проверка.")
                         : QStringLiteral("Деталь и инструмент выбраны. Можно проверить.");
        if (preview && !preparing) {
            if (preview->floorChecked())
                result += QStringLiteral(
                    "\nДемонстрационный станок и пол · стол 900 мм. Проверка по кадрам; "
                    "ход и перенос не проверены.");
            else if (preview->machineModel() != core::MachineModel::None)
                result += QStringLiteral(
                    "\nДемонстрационный станок: проверка по кадрам. Пол не проверен; "
                    "добавьте его на этапе «Инструмент».");
            else if (preview->toolSetup())
                result += QStringLiteral("\nСтанок и пол не проверены.");
            if ((preview->machineModel() == core::MachineModel::DemoPressV2 ||
                 preview->machineModel() == core::MachineModel::DemoPressV3) &&
                preview->usesDrawingPunch() && !preview->seatedPunchMount())
                result += QStringLiteral("\nХвостовик не распознан; посадка условная.");
            if (preview->ignoresClosedCutouts())
                result += QStringLiteral("\nПроверка по копии без вырезов.");
            if (preview->toolSetup() &&
                (!preview->usesDrawingPunch() || !preview->usesDrawingDie()))
                result += QStringLiteral("\nЕсть условные профили инструмента.");
            if (preview->toolSegmentGaps()[0] > 0 || preview->toolSegmentGaps()[1] > 0)
                result += QStringLiteral("\nВлияние зазоров секций не проверено.");
            if (viewingRoute && route && routePanel->operation() > 0) {
                const auto *current = session.bend(route->steps[routePanel->operation()].bendIndex);
                const auto *previous =
                    session.bend(route->steps[routePanel->operation() - 1].bendIndex);
                if (current && previous &&
                    (current->tools != previous->tools ||
                     current->catalogTools != previous->catalogTools))
                    result += QStringLiteral("\nПереналадка не проверена.");
            }
        }
        const auto primaryMessage = result;
        const auto draft = viewingDraft ? controller->routeDraft() : nullptr;
        const bool shortTools = draft && draft->toolLengths && !preparing;
        const auto lengthPlaybackNotice =
            shortTools
                ? (shortToolPlaybackBlocked() ? QStringLiteral("Воспроизведение заблокировано.")
                                              : QStringLiteral("Диагностический просмотр."))
                : QString{};
        // Length remains visible even when a collision/floor message takes priority.
        if (shortTools)
            result += QLatin1Char('\n') + ToolSelectionWidget::lengthIssue(*draft->toolLengths) +
                      QLatin1Char('\n') + lengthPlaybackNotice;
        if (bendMessages && !preparing) {
            std::vector<BendMessage> messages;
            const auto lengths = draft && draft->toolLengths ? draft->toolLengths
                                 : controller->bendRouteError()
                                     ? controller->bendRouteError()->toolLengths
                                     : std::nullopt;
            if (collision)
                messages.push_back({BendMessageKind::Collision, BendMessageSeverity::Error,
                                    machineCollision && machineCollision->secondId == "floor"
                                        ? QStringLiteral("Столкновение с полом")
                                        : QStringLiteral("Столкновение"),
                                    primaryMessage});
            else if (!lengths && (!stageSetupIssue.isEmpty() || !previewDisplayError.isEmpty() ||
                                  controller->routeDraftError() || controller->bendRouteError() ||
                                  controller->bendPreviewError())) {
                const auto routeError = controller->bendRouteError();
                const bool limited =
                    routeError &&
                    (routeError->code == application::BendRouteErrorCode::SearchLimit ||
                     routeError->code == application::BendRouteErrorCode::NoRoute);
                messages.push_back(
                    {BendMessageKind::CheckIssue,
                     limited ? BendMessageSeverity::Warning : BendMessageSeverity::Error,
                     limited ? QStringLiteral("Порядок пока не найден")
                             : QStringLiteral("Проверка не пройдена"),
                     primaryMessage});
            }
            if (lengths) {
                const bool punchShort = lengths->punchLengthMm < lengths->bendLengthMm;
                const bool dieShort = lengths->dieLengthMm < lengths->bendLengthMm;
                const auto lengthTitle =
                    punchShort && dieShort ? QStringLiteral("Короткий пуансон и матрица")
                    : punchShort ? QStringLiteral("Пуансон короче на %1 мм")
                                       .arg(number(lengths->bendLengthMm - lengths->punchLengthMm))
                                 : QStringLiteral("Матрица короче на %1 мм")
                                       .arg(number(lengths->bendLengthMm - lengths->dieLengthMm));
                messages.push_back(
                    {BendMessageKind::ToolLength,
                     draft ? BendMessageSeverity::Warning : BendMessageSeverity::Error, lengthTitle,
                     ToolSelectionWidget::lengthIssue(*lengths) +
                         (lengthPlaybackNotice.isEmpty()
                              ? QString{}
                              : QLatin1Char('\n') + lengthPlaybackNotice)});
            }
            if (partial)
                messages.push_back({BendMessageKind::Support, BendMessageSeverity::Warning,
                                    QStringLiteral("Неполная опора у торцов"),
                                    QStringLiteral("У торца отсутствует часть опоры. Доступен "
                                                   "геометрический просмотр; устойчивость и "
                                                   "пригодность к изготовлению не подтверждены.")});
            if (preview && preview->backgauge()) {
                const auto &g = *preview->backgauge();
                QString title = QStringLiteral("База упора не найдена");
                QString detail = QString::fromStdString(g.detail);
                if (g.placement) {
                    const auto &p = *g.placement;
                    title = p.kind == core::GaugeReferenceKind::StraightEdge
                                ? QStringLiteral("Упор по прямому краю")
                                : QStringLiteral("Упор по стенке 90°");
                    detail = QStringLiteral("Ступень %1 · X %2 · R %3 · Z %4 / %5 мм\n%6")
                                 .arg(p.level)
                                 .arg(number(p.xMm))
                                 .arg(number(p.rMm))
                                 .arg(number(p.zMm[0]))
                                 .arg(number(p.zMm[1]))
                                 .arg(detail);
                }
                messages.push_back({BendMessageKind::Backgauge,
                                    g.status == application::BackgaugeStatus::Checked
                                        ? BendMessageSeverity::Information
                                        : BendMessageSeverity::Warning,
                                    title, detail});
            }
            if (preview && preview->ignoresClosedCutouts())
                messages.push_back(
                    {BendMessageKind::Approximation, BendMessageSeverity::Warning,
                     QStringLiteral("Отверстия исключены из проверки"),
                     QStringLiteral(
                         "Отверстия видимы. Проверки относятся к отдельной "
                         "копии с заполненными вырезами; их физическое влияние не рассчитано.")});
            if (messages.empty() && preview && preview->unverifiedDraft())
                messages.push_back({BendMessageKind::Scope,
                                    draft && !draft->checkedToolPositions
                                        ? BendMessageSeverity::Warning
                                        : BendMessageSeverity::Information,
                                    draft && !draft->checkedToolPositions
                                        ? QStringLiteral("Инструмент не выбран")
                                        : QStringLiteral("Просмотр шага · порядок не проверен"),
                                    primaryMessage});
            bendMessages->setMessages(std::move(messages));
        }
        checkSummary->setText(result);
        checkSummary->setStyleSheet(
            collision && !preparing
                ? QStringLiteral("background:#FBE9E7;color:#8F2318;border-radius:4px;padding:10px;")
            : partial || shortTools || controller->bendRouteError() || controller->routeDraftError()
                ? QStringLiteral("background:#FFF2CC;color:#704500;border-radius:4px;padding:10px;")
                : QStringLiteral(
                      "background:#F0F3F6;color:#1C2938;border-radius:4px;padding:10px;"));
        checkSummary->setMinimumHeight(0);
        const auto index = selectedOperationIndex();
        const auto punchIndex = selectedPunchIndex();
        const auto *selected = punchIndex ? session.bend(*punchIndex) : nullptr;
        const auto *selectedTools = selected && selected->tools ? &*selected->tools : nullptr;
        if (reversePunchButton) {
            const QSignalBlocker block(reversePunchButton);
            reversePunchButton->setEnabled(punchIndex && selected && included(selected->review) &&
                                           selectedTools && !controller->hasWork());
            const bool reversed = selectedTools && selectedTools->punchReversed;
            reversePunchButton->setChecked(reversed);
            reversePunchButton->setIcon(punchOrientationIcons[reversed ? 1 : 0]);
            const auto scope = blankSetupSelected()
                                   ? QStringLiteral("\nШаг 0: для всех гибов с инструментом.")
                                   : QStringLiteral("\nТолько выбранный гиб.");
            std::optional<bool> frontPreference;
            if (selected && selected->catalogTools) {
                const auto &assigned = *selected->catalogTools;
                if (!facingPunch || *facingPunch != assigned.punch) {
                    facingPunch = assigned.punch;
                    facingPreference = core::defaultPunchOrientation(assigned.punch);
                }
                frontPreference = facingPreference;
            }
            const auto facing =
                frontPreference
                    ? (reversed == *frontPreference ? QStringLiteral("Выемка к открытому зеву.")
                                                    : QStringLiteral("Выемка к задней раме."))
                    : (reversed ? QStringLiteral("Пуансон: положение 2.")
                                : QStringLiteral("Пуансон: положение 1."));
            reversePunchButton->setToolTip(
                facing + QStringLiteral(" Нажмите, чтобы развернуть на 180°.") + scope);
            reversePunchButton->setAccessibleDescription(
                reversed ? QStringLiteral("Развёрнутая установка")
                         : QStringLiteral("Обычная установка"));
        }
        if (reverseWorkpieceButton) {
            const QSignalBlocker block(reverseWorkpieceButton);
            const auto *part = punchIndex ? session.bend(*punchIndex) : nullptr;
            const bool reversed = part && part->workpieceReversed;
            reverseWorkpieceButton->setEnabled(part && preview && preview->toolSetup() &&
                                               !controller->hasWork());
            reverseWorkpieceButton->setChecked(reversed);
            reverseWorkpieceButton->setIcon(workpieceOrientationIcons[reversed ? 1 : 0]);
            reverseWorkpieceButton->setToolTip(
                QStringLiteral(
                    "Деталь: %1. Развернуть на 180° вокруг вертикали Y.\n"
                    "Инструмент остаётся на месте. Только текущий гиб; шаг 0 — первый гиб.\n"
                    "«Проверить мой порядок» сохраняет положение; «Подобрать порядок» может его "
                    "изменить.")
                    .arg(reversed ? QStringLiteral("180°") : QStringLiteral("0°")));
        }
        operationSummary->clear();
        if (const auto &document = session.document(); document) {
            if (index && *index < document->metadata().bendAnalysis.bends.size()) {
                const auto &bend = document->metadata().bendAnalysis.bends[*index];
                const auto target = session.bendTarget(*index);
                operationSummary->setText(
                    QStringLiteral("Гиб %1 из %2\n%3° · L %4 мм")
                        .arg(document->metadata().bendAnalysis.bends.size() > 1
                                 ? routePanel->selected()
                                 : 1)
                        .arg(session.orderedBends().size())
                        .arg(number((std::numbers::pi -
                                     (target ? target->angleRadians : bend.angleRadians)) *
                                    180 / std::numbers::pi),
                             number(bend.lengthMm)));
                if (const auto *entry = session.bend(*index); entry && entry->tools) {
                    const auto name = entry->catalogTools
                                          ? text(entry->catalogTools->punch.code) + " / " +
                                                text(entry->catalogTools->die.code)
                                          : QStringLiteral("Инструмент");
                    operationSummary->setText(operationSummary->text() +
                                              QStringLiteral("\n%1 · V %2 мм")
                                                  .arg(name, number(entry->tools->openingMm)));
                }
            } else
                operationSummary->setText(
                    QStringLiteral("Заготовка · гибов %1").arg(session.orderedBends().size()));
        }
    }
    application::BendPreviewPtr scenePreview() const {
        if (viewingDraft) {
            const auto draft = controller->routeDraft();
            return draft ? draft->visual : nullptr;
        }
        if (viewingRoute) {
            if (const auto route = controller->bendRoute(); route && !route->steps.empty())
                return route->steps[std::min(routePanel->operation(), route->steps.size() - 1)]
                    .preview;
            return nullptr;
        }
        if (const auto p = controller->bendPreview())
            return p;
        const auto &e = controller->bendPreviewError();
        return e ? e->diagnostic : nullptr;
    }
    void prepareManualDraft() {
        const auto &session = controller->session();
        if (!session.document() || !previewBuilder)
            return;
        stageSetupIssue.clear();
        stageToolIssueIndex.reset();
        beginPreviewReplacement();
        lastRouteAutomatic = false;
        viewer->highlightPreviewBend(std::nullopt);
        application::RouteDraftRequest request;
        request.step = static_cast<std::size_t>(routePanel->selected());
        // Manual preview never offers a switch that suppresses tool intersections.
        request.withTools = true;
        if (toolDraftChanged && !commitStageTools())
            return;
        if (const auto index = routePanel->selectedBend()) {
            const QSignalBlocker block(bendList->selectionModel());
            bendList->setCurrentIndex(bendModel->index(static_cast<int>(*index), 0));
        }
        for (auto index : session.orderedBends()) {
            const auto *entry = session.bend(index);
            const auto target = session.bendTarget(index);
            if (!entry || !included(entry->review) || !target)
                return;
            const auto tools = entry->tools;
            const auto assignment = entry->catalogTools;
            // Explicit geometry-only inspection has no tool body. Existing/partial tools
            // must never be replaced with a generated wedge.
            if ((tools || assignment) && !requireRealTool(assignment, index))
                return;
            request.operations.push_back({entry->id, index, *target, entry->tools,
                                          entry->catalogTools, entry->workpieceReversed});
        }
        stopPlayback();
        viewingDraft = true;
        viewingRoute = false;
        viewingSource = false;
        wantFlat = false;
        positionedPreview.reset();
        if (controller->prepareRouteDraft(std::move(request), previewBuilder))
            showPlaybackPanel();
    }
    void showPreviewFrame() {
        const auto preview = scenePreview();
        if (!preview || viewingSource) {
            return;
        }
        wantFlat = false;
        displayedFlat.reset();
        if (shortToolPlaybackBlocked() && bendFrame->value() != 0) {
            const QSignalBlocker block(bendFrame);
            bendFrame->setValue(0);
        }
        const auto index = static_cast<std::size_t>(bendFrame->value());
        if (!viewingDraft && !viewingRoute && preview->pressMotion() &&
            controller->session().orderedBends().size() == 1)
            routePanel->select(index == 0 ? 0 : 1, false);
        if (displayedPreview != preview || displayedPreviewFrame != index ||
            displayedPreviewRevision != controller->session().revision()) {
            const auto failure =
                viewer->presentBendPreview(preview, index, controller->session().revision());
            if (!failure.isEmpty()) {
                previewDisplayError = failure;
                stopPlayback();
                previewStatus->setText(
                    QStringLiteral("Не удалось показать состояние: %1").arg(failure));
                return;
            }
            displayedPreview = preview;
            displayedPreviewFrame = index;
            displayedPreviewRevision = controller->session().revision();
        }
        previewReplacement.reset();
        previewDisplayError.clear();
        if (collisionAt(preview, index)) {
            stopPlayback();
            draftAutoPlay = false;
        }
        refreshPlaybackAvailability(preview);
        refreshCheckResult();
        const double progressValue =
            preview->frameCount() > 1
                ? static_cast<double>(index) / static_cast<double>(preview->frameCount() - 1)
                : 0.0;
        const auto approximation =
            preview->ignoresClosedCutouts() ? QStringLiteral("Проверка без вырезов · ") : QString{};
        const auto gaps = preview->toolSegmentGaps();
        const auto segmented = (gaps[0] > 0 || gaps[1] > 0)
                                   ? QStringLiteral("Зазоры инструмента — без расчёта влияния · ")
                                   : QString{};
        if (preview->unverifiedDraft()) {
            const auto draft = controller->routeDraft();
            const bool collision = collisionAt(preview, index);
            const auto message =
                collision
                    ? QStringLiteral(
                          "Столкновение — место отмечено красным, воспроизведение остановлено")
                : draft && draft->checkedToolPositions
                    ? QStringLiteral("В этом положении пересечений с инструментом не найдено")
                    : QStringLiteral("Инструмент не задан — столкновения не проверены");
            setFrameText(QStringLiteral("Ручной просмотр · %1% · %2")
                             .arg(qRound(progressValue * 100))
                             .arg(message));
            return;
        }
        if (collisionAt(preview, index)) {
            setFrameText(QStringLiteral("Обнаружено столкновение · диагностическое положение"));
            return;
        }
        if (const auto *state = preview->pressState(index)) {
            setFrameText(
                approximation + segmented +
                QStringLiteral("%1 · %2% · поворот %3° · ход пуансона %4 мм")
                    .arg(state->phase == simulation::PressPhase::Approach ? QStringLiteral("Подвод")
                         : state->phase == simulation::PressPhase::Retracting
                             ? QStringLiteral("Отвод пуансона")
                             : QStringLiteral("Гибка"),
                         QLocale::system().toString(progressValue * 100.0, 'f', 0),
                         QLocale::system().toString(
                             state->section.angleRadians * 180.0 / std::numbers::pi, 'f', 1),
                         QLocale::system().toString(state->punchTravelMm, 'f', 1)));
            return;
        }
        setFrameText(approximation +
                     (preview->toolSetup()
                          ? QStringLiteral("Начальное положение · статичный просмотр")
                          : QStringLiteral("%1% · поворот %2°")
                                .arg(number(progressValue * 100.0),
                                     number(progressValue * preview->model().angleRadians * 180.0 /
                                            std::numbers::pi))));
    }
    void refreshPreview() {
        const auto &session = controller->session();
        const auto &document = session.document();
        if (previewReplacement &&
            (previewReplacement->document != document ||
             previewReplacement->revision != session.revision() || viewingSource))
            previewReplacement.reset();
        const bool multiple = document && document->metadata().bendAnalysis.bends.size() > 1;
        if (toolScopeRevision != session.revision()) {
            toolScopeRevision = session.revision();
            toolContextId.clear();
            toolDraftChanged = false;
            lastRouteAutomatic = true;
            routePanel->setWithTools(true);
        }
        toolAssignmentScope->setVisible(multiple);
        toolAssignmentScope->setEnabled(!controller->hasWork());
        {
            const QSignalBlocker block(toolAssignmentScope);
            toolAssignmentScope->clear();
            toolAssignmentScope->addItem(QStringLiteral("Вся деталь"), QString{});
            int position = 0;
            for (const auto index : session.orderedBends()) {
                const auto *entry = session.bend(index);
                toolAssignmentScope->addItem(QStringLiteral("Гиб %1").arg(++position),
                                             text(entry->id));
            }
            auto at = toolAssignmentScope->findData(text(toolContextId));
            if (at < 0) {
                toolContextId.clear();
                at = 0;
            }
            toolAssignmentScope->setCurrentIndex(at);
        }
        const bool perBend = !toolContextId.empty();
        assignBendTool->setVisible(toolDraftChanged);
        toolAssignmentStatus->setVisible(multiple && perBend);
        toolAssignmentStatus->setText(QStringLiteral("Переналадка между гибами не проверяется."));
        routePanel->setVisible(propertyTabs->currentIndex() == 2 && document &&
                               document->metadata().bendAnalysis.bends.size() > 1);
        const auto stagePreview = scenePreview();
        const bool singleCycle =
            !multiple && document &&
            ((stagePreview && stagePreview->pressMotion()) ||
             (controller->isPreparingPreview() && toolMode->currentIndex() == 0));
        routePanel->stepStrip()->setVisible(propertyTabs->currentIndex() == 2 && document &&
                                            (multiple || singleCycle));
        routePanel->refresh(session, controller->bendRoute(), controller->bendRouteError(),
                            controller->hasWork() &&
                                (!controller->isPreparingDraft() || session.activeRequest()),
                            controller->isPreparingRoute(), toolSelection->parameters().has_value(),
                            controller->routeDraft(), controller->routeDraftError(),
                            controller->isPreparingDraft());
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        const bool hasInteriorContours =
            document && !document->metadata().bendAnalysis.interiorContourDiagnostics.empty() &&
            document->metadata().closedCutouts.status != ClosedCutoutStatus::Filled;
        bool supported =
            document && !hasInteriorContours && row >= 0 &&
            static_cast<std::size_t>(row) < document->metadata().bendAnalysis.bends.size() &&
            !document->metadata().bendAnalysis.bends[static_cast<std::size_t>(row)].trimmedEnds;
        bool matchingThickness = true;
        if (supported && session.manualThicknessMm()) {
            matchingThickness =
                std::abs(*session.manualThicknessMm() -
                         document->metadata()
                             .bendAnalysis.bends[static_cast<std::size_t>(row)]
                             .thicknessMm) <= document->metadata().bendAnalysis.lengthToleranceMm;
        }
        preparePreview->setEnabled(
            previewBuilder && supported && matchingThickness && entry && included(entry->review) &&
            session.bendTarget(static_cast<std::size_t>(row)) && !controller->hasWork());
        fixedSide->setEnabled(document && !controller->hasWork());
        toolMode->setEnabled(document && !controller->hasWork());
        const bool toolEditable = entry && included(entry->review) && !controller->hasWork();
        const bool allBends = multiple && !perBend && toolMode->currentIndex() == 0;
        std::optional<core::ToolSelectionRequest> selectionRequest;
        if (document && entry && included(entry->review) && matchingThickness && row >= 0 &&
            static_cast<std::size_t>(row) < document->metadata().bendAnalysis.bends.size()) {
            const auto &bend =
                document->metadata().bendAnalysis.bends[static_cast<std::size_t>(row)];
            if (const auto target = session.bendTarget(static_cast<std::size_t>(row)))
                selectionRequest = core::ToolSelectionRequest{
                    bend.thicknessMm, target->angleRadians, target->innerRadiusMm, bend.lengthMm};
            if (allBends && selectionRequest) {
                const auto &bends = document->metadata().bendAnalysis.bends;
                for (std::size_t i = 0; i < bends.size(); ++i)
                    if (const auto target = session.bendTarget(i)) {
                        selectionRequest->bendLengthMm =
                            std::max(selectionRequest->bendLengthMm, bends[i].lengthMm);
                        selectionRequest->bendAngleRadians =
                            std::max(selectionRequest->bendAngleRadians, target->angleRadians);
                        selectionRequest->innerRadiusMm =
                            std::min(selectionRequest->innerRadiusMm, target->innerRadiusMm);
                    }
            }
        }
        toolSelection->setSelectionRequest(
            selectionRequest, allBends,
            document ? document->metadata().bendAnalysis.lengthToleranceMm : 1.e-5);
        if (checkedToolJob && controller->hasWork() && !controller->isPreparingRoute()) {
            // Toolbar cancellation or another command can stop the worker too.
            checkedToolCancelled = true;
            checkedToolVariants.clear();
            checkedToolDialog->hide();
        }
        if (checkedToolDocument != session.revision() &&
            (checkedToolJob || !checkedToolVariants.empty())) {
            checkedToolVariants.clear();
            checkedToolCancelled = true;
            checkedToolDialog->hide();
            if (!controller->isPreparingRoute()) {
                checkedToolJob = false;
                toolSelection->setCheckedSearchState(false);
            }
        }
        toolSelection->setEnabled(!controller->hasWork() || checkedToolJob);
        toolWorkspace->setEnabled(!controller->hasWork() || checkedToolJob);
        applyTools->setEnabled(toolEditable && previewBuilder && supported && matchingThickness &&
                               session.bendTarget(static_cast<std::size_t>(row)).has_value());
        checkBend->setEnabled(applyTools->isEnabled());
        assignBendTool->setText(QStringLiteral("Применить выбор"));
        clearTools->setEnabled(toolEditable && entry->tools.has_value());
        const auto context = toolContextIndex();
        const auto *toolEntry = context ? session.bend(*context) : nullptr;
        const auto entryId = toolEntry ? toolEntry->id : std::string{};
        const auto savedTools = toolEntry ? toolEntry->tools : std::nullopt;
        const auto savedAssignment = toolEntry ? toolEntry->catalogTools : std::nullopt;
        const bool acceptedToolSearch = checkedToolJob && !controller->hasWork() &&
                                        !checkedToolManualOverride && controller->bendRoute() &&
                                        controller->bendRoute()->selectedToolVariant.has_value();
        const bool savedChoiceChanged =
            shownTools != savedTools || shownAssignment != savedAssignment;
        if (toolsDocumentRevision != session.revision() ||
            (savedChoiceChanged && toolsEntryId == entryId) ||
            ((!toolDraftChanged || acceptedToolSearch) &&
             (toolsEntryId != entryId || savedChoiceChanged))) {
            toolsDocumentRevision = session.revision();
            toolsEntryId = entryId;
            shownTools = savedTools;
            shownAssignment = savedAssignment;
            toolDraftChanged = false;
            toolDraftPunchSelected = toolDraftDieSelected = false;
            toolSelection->setChoice(savedTools, savedAssignment);
        }
        if (checkedToolJob && !controller->hasWork()) {
            checkedToolJob = false;
            QString status;
            bool resumable = false;
            if (checkedToolCancelled) {
                checkedToolDialog->hide();
                if (!checkedToolManualOverride)
                    status = QStringLiteral("Подбор отменён");
            } else if (const auto route = controller->bendRoute();
                       route && route->selectedToolVariant) {
                checkedToolDialog->hide();
                checkedToolVariants.clear();
                toolSelection->rememberChoice();
                status = QStringLiteral("Проверенный порядок · %1 гибов").arg(route->steps.size());
            } else {
                const auto routeError = controller->bendRouteError();
                resumable =
                    routeError && routeError->code == application::BendRouteErrorCode::SearchLimit;
                if (!resumable)
                    checkedToolVariants.clear();
                status =
                    resumable
                        ? QStringLiteral("Проверка не завершена. Продолжите подбор или выберите "
                                         "инструмент вручную.")
                        : QStringLiteral(
                              "Проверенный вариант не найден. Доступен быстрый и ручной выбор.");
                checkedToolDialog->finish(
                    resumable,
                    routeError && routeError->operationError ==
                                      application::BendPreviewErrorCode::BackgaugeUnavailable
                        ? QString::fromStdString(routeError->detail)
                        : QString{});
            }
            toolSelection->setCheckedSearchState(false, status, resumable);
        } else if (checkedToolJob) {
            if (const auto routeProgress = controller->bendRouteProgress()) {
                QString tool;
                if (routeProgress->variant < checkedToolVariants.size()) {
                    const auto &a = checkedToolVariants[routeProgress->variant].assignment;
                    tool = QString::fromStdString(a.punch.code + " / " + a.die.code);
                    if (a.groove < a.die.openingsMm.size())
                        tool += QStringLiteral(" · V %1 мм").arg(a.die.openingsMm[a.groove]);
                }
                checkedToolDialog->progress(*routeProgress, tool);
            }
        }
        assignBendTool->setEnabled(toolEditable && supported && matchingThickness &&
                                   toolSelection->parameters().has_value());
        toolWorkspace->setContext(multiple ? toolAssignmentScope->currentText() : QString{});
        toolWorkspace->refresh();
        const auto preview = scenePreview();
        const auto draft = viewingDraft ? controller->routeDraft() : nullptr;
        const bool shortDraft = draft && draft->toolLengths;
        if (!shortDraft || viewingSource || propertyTabs->currentIndex() != 2 ||
            controller->hasWork() || shortToolDiagnosticDraft.lock() != draft)
            shortToolDiagnosticDraft.reset();
        {
            const QSignalBlocker block(shortToolDiagnostic);
            shortToolDiagnostic->setChecked(shortDraft && shortToolDiagnosticDraft.lock() == draft);
        }
        shortToolDiagnostic->setVisible(shortDraft && draft->request.step > 0);
        shortToolDiagnostic->setEnabled(shortDraft && !controller->hasWork());
        const bool diagnostic =
            preview && !preview->unverifiedDraft() &&
            (preview->collision() || preview->machineCollisionAtFrame(preview->frameCount() - 1));
        if (!preview) {
            previewDisplayError.clear();
            positionedPreview.reset();
        }
        if (!preview && displayedPreview && !previewReplacement &&
            !(viewingDraft && controller->isPreparingDraft())) {
            stopPlayback();
            displayedPreview.reset();
            const auto failure = viewer->endBendPreview();
            if (!failure.isEmpty()) {
                previewStatus->setToolTip(failure);
            }
        }
        if (preview &&
            (displayedPreview != preview || positionedPreview != preview ||
             (shortToolPlaybackBlocked() && bendFrame->value() != 0)) &&
            !viewingSource) {
            if (positionedPreview != preview) {
                const QSignalBlocker block(bendFrame);
                bendFrame->setRange(0, static_cast<int>(preview->frameCount() - 1));
                const bool restoreFrame =
                    previewReplacement && previewReplacement->preserveFrame &&
                    previewReplacement->previous->frameCount() == preview->frameCount() &&
                    previewReplacement->previous->unverifiedDraft() == preview->unverifiedDraft() &&
                    bool(previewReplacement->previous->pressMotion()) ==
                        bool(preview->pressMotion());
                std::optional<int> matchedAngle;
                if (!restoreFrame && previewReplacement && previewReplacement->preserveFrame) {
                    const auto *old =
                        previewReplacement->previous->pressState(previewReplacement->frame);
                    double best = std::numeric_limits<double>::infinity();
                    if (old)
                        for (std::size_t f = 0; f < preview->frameCount(); ++f)
                            if (const auto *state = preview->pressState(f)) {
                                const double distance = std::abs(state->section.angleRadians -
                                                                 old->section.angleRadians);
                                if (distance < best) {
                                    best = distance;
                                    matchedAngle = static_cast<int>(f);
                                }
                            }
                }
                bendFrame->setValue(restoreFrame   ? static_cast<int>(previewReplacement->frame)
                                    : matchedAngle ? *matchedAngle
                                    : diagnostic   ? bendFrame->maximum()
                                    : (viewingDraft || viewingRoute) &&
                                            routePanel->selected() > 0 && !draftAutoPlay
                                        ? bendFrame->maximum()
                                        : 0);
                positionedPreview = preview;
            }
            showPreviewFrame();
        }
        const bool staticSetup =
            preview &&
            ((diagnostic && preview->frameCount() == 1) ||
             (!preview->unverifiedDraft() && preview->toolSetup() && !preview->pressMotion()));
        previewControls->setCurrentIndex(staticSetup ? 1 : 0);
        kinematicsToggle->setVisible(!viewingDraft);
        kinematicsPanel->setVisible(!viewingDraft && kinematicsToggle->isChecked());
        playBend->setVisible(!staticSetup);
        playbackSpeedControls->setVisible(!staticSetup);
        showKinematics->setVisible(staticSetup && !diagnostic);
        showKinematics->setEnabled(staticSetup && preparePreview->isEnabled());
        bendFrame->setEnabled(preview && !viewingSource && !staticSetup && !controller->hasWork() &&
                              !shortToolPlaybackBlocked() &&
                              !((viewingRoute || viewingDraft) && routePanel->selected() == 0) &&
                              previewDisplayError.isEmpty());
        refreshPlaybackAvailability(preview);
        returnToCad->setEnabled(preview || previewReplacement || controller->isPreparingPreview() ||
                                controller->isPreparingDraft());
        if (!preview || viewingSource || staticSetup || controller->hasWork()) {
            stopPlayback();
        }
        if (controller->isPreparingDraft()) {
            if (!previewReplacement)
                setFrameText(QStringLiteral("Готовим выбранный шаг и проверяем столкновения…"));
            previewStatus->setText(QStringLiteral(
                "Ручной порядок: проверяется выбранный переход без поиска последовательности."));
        } else if (preview && preview->unverifiedDraft()) {
            previewStatus->setText(
                QStringLiteral("Ручной порядок. Столкновения с инструментом проверяются в каждом "
                               "подготовленном кадре. "
                               "Опора, непрерывная траектория между кадрами, отвод и весь порядок "
                               "не подтверждены. "
                               "Для полной проверки нажмите «Проверить мой порядок»."));
        } else if (controller->isPreparingPreview()) {
            if (!previewReplacement)
                setFrameText(QStringLiteral("Подготовка просмотра…"));
            previewStatus->setText(
                QStringLiteral("Проверяем восстановление всей детали и готовим геометрию…"));
        } else if (const auto &failure = controller->bendPreviewError()) {
            previewStatus->setText(
                failure->code == BendPreviewErrorCode::TargetMismatch
                    ? QStringLiteral("Для первого просмотра целевые угол и радиус должны совпадать "
                                     "с STEP. Верните параметры из CAD.")
                : failure->code == BendPreviewErrorCode::ToolTooShort
                    ? QStringLiteral("Инструмент короче ширины заготовки. Увеличьте рабочую длину.")
                : failure->code == BendPreviewErrorCode::MissingLeftSupport
                    ? QStringLiteral("Под левой полкой нет опоры: край не перекрывает плечо "
                                     "матрицы. Уменьшите раскрытие V.")
                : failure->code == BendPreviewErrorCode::MissingRightSupport
                    ? QStringLiteral("Под правой полкой нет опоры: край не перекрывает плечо "
                                     "матрицы. Уменьшите раскрытие V.")
                : failure->code == BendPreviewErrorCode::ToolAngleConflict
                    ? QStringLiteral("Угол пуансона или паза слишком велик для целевого сгиба. "
                                     "Уменьшите угол инструмента.")
                : failure->code == BendPreviewErrorCode::NoseTooLarge
                    ? QStringLiteral("Радиус носика должен быть меньше внутреннего радиуса сгиба. "
                                     "Конформный контакт пока не поддерживается.")
                : failure->code == BendPreviewErrorCode::EndSupportLimitExceeded &&
                        failure->endSupport
                    ? QStringLiteral(
                          "Нехватка опоры у торцов %1 / %2 мм превышает допуск %3 мм. "
                          "Проверьте матрицу или измените допуск в параметрах установки.")
                          .arg(failure->endSupport->supportedFromMm, 0, 'f', 2)
                          .arg(failure->endSupport->bendLengthMm -
                                   failure->endSupport->supportedToMm,
                               0, 'f', 2)
                          .arg(toolSelection->maximumEndSupportLossMm(), 0, 'f', 2)
                : failure->code == BendPreviewErrorCode::SupportNotProven
                    ? QStringLiteral(
                          "Не удалось подтвердить сплошной участок опоры обеих полок: проверьте "
                          "отверстия/контур около сгиба и раскрытие V. Для движения участок "
                          "опоры проверяется на всём ходе.")
                : failure->code == BendPreviewErrorCode::MachineIntersection
                    ? QStringLiteral("Столкновение с узлом демонстрационного станка: %1")
                          .arg(text(failure->detail))
                : failure->code == BendPreviewErrorCode::ToolIntersection
                    ? QStringLiteral("Обнаружено столкновение с инструментом в положении %1. "
                                     "Конфликтующие тела выделены красным.")
                          .arg(failure->frameIndex.value_or(0) + 1)
                : failure->code == BendPreviewErrorCode::SheetIntersection
                    ? QStringLiteral("Части детали пересекаются при раскрытии выбранного гиба. "
                                     "Эта зона не подтверждена как последняя операция; нужен "
                                     "другой порядок гибов.")
                : failure->code == BendPreviewErrorCode::ContactLost
                    ? QStringLiteral("Не удалось подтвердить рабочий контакт в положении %1. "
                                     "Просмотр движения не подготовлен.")
                          .arg(failure->frameIndex.value_or(0) + 1)
                : failure->code == BendPreviewErrorCode::RetractionNotProven
                    ? QStringLiteral(
                          "Не удалось подтвердить свободный отвод пуансона. "
                          "Полный цикл не подготовлен; проверьте инструмент и геометрию.")
                : failure->code == BendPreviewErrorCode::NarrowDieOpening
                    ? QStringLiteral(
                          "Раскрытие V слишком мало для толщины детали. Выберите подходящую "
                          "матрицу; ориентир обычной свободной гибки — около 8t.")
                : failure->code == BendPreviewErrorCode::UnsupportedToolGeometry
                    ? QStringLiteral(
                          "Для выбранной формы DXF не удалось подтвердить рабочие участки. Нужны "
                          "поддержанные рабочие участки у каждого загруженного контура.")
                : failure->code == BendPreviewErrorCode::UnsupportedParameters
                    ? QStringLiteral("Параметры вне диапазона первого сценария: поворот 5–175°, "
                                     "толщина/радиус 0,1–100000 мм, ширина/полки 1–100000 мм.")
                : failure->code == BendPreviewErrorCode::BendZoneInteriorContours
                    ? QStringLiteral("В зоне сгиба обнаружены внутренние контуры отверстия или "
                                     "кармана. Эта геометрия пока не поддерживается для движения. "
                                     "Выберите диагностику D слева для подсветки.")
                : failure->code == BendPreviewErrorCode::UnsupportedGeometry
                    ? QStringLiteral(
                          "Не удалось выделить две части детали по выбранному полному прямому "
                          "гибу. "
                          "Полная геометрия должна пройти проверку восстановления; "
                          "замкнутые связи и подрезанные зоны не раскрываются произвольно.")
                    : QStringLiteral("Не удалось подготовить кинематический просмотр. Исходная "
                                     "деталь сохранена."));
            previewStatus->setToolTip(text(failure->detail));
            operationFailureSummary = previewStatus->text();
        } else if (!previewDisplayError.isEmpty()) {
            previewStatus->setText(
                QStringLiteral("Не удалось показать состояние: %1").arg(previewDisplayError));
        } else if (preview && preview->pressMotion() && preview->punchRetraction()) {
            const auto &report = *preview->punchRetraction();
            previewStatus->setText(
                QStringLiteral("Цикл с инструментом подготовлен\nПоложений просмотра: %1\n"
                               "Подвод → гибка на двух опорах → отвод пуансона. "
                               "Нажмите «Воспроизвести» или используйте ползунок.\n\n"
                               "Отвод вверх: %2 мм\nКонечный минимальный зазор до детали: %3 мм\n\n"
                               "Подвод и гибка проверены по кадрам. Весь отвод пуансона проверен "
                               "непрерывно по проходящему объёму. "
                               "Усилие, пружинение и извлечение детали не рассчитываются.")
                    .arg(preview->frameCount())
                    .arg(number(report.liftMm), number(report.finalClearanceMm)));
            previewStatus->setToolTip({});
        } else if (preview && preview->toolSetup()) {
            const auto &setup = *preview->toolSetup();
            previewStatus->setText(
                QStringLiteral("Начальная установка проверена\nПоказано одно неподвижное "
                               "положение.\nПерекрытие плеч матрицы: слева %1 "
                               "мм, справа %2 мм.\n"
                               "Зазор до пуансона: %3 мм. Объёмных пересечений не обнаружено в "
                               "пределах допуска.\n\n"
                               "Проверено только это положение. Движение, рабочий контакт при "
                               "гибке, траектории установки и извлечения ещё не проверяются.")
                    .arg(number(setup.leftOverlapMm), number(setup.rightOverlapMm),
                         number(setup.tools.clearanceMm)));
            previewStatus->setToolTip({});
        } else if (preview) {
            const auto &m = preview->model();
            previewStatus->setText(
                QStringLiteral(
                    "Местные габариты у сгиба: %1 × %2 × %3 мм\nГабарит стороны A: %4 "
                    "мм\nГабарит стороны B: %5 "
                    "мм\nДуга средней поверхности: %6 мм\n\nСиняя — неподвижная полка; оранжевая — "
                    "зона сгиба; зелёная — подвижная полка.")
                    .arg(number(simulation::blankLengthMm(m)), number(m.widthMm),
                         number(m.thicknessMm), number(m.fixedFlangeMm), number(m.movingFlangeMm),
                         number(simulation::bendAllowanceMm(m))));
            previewStatus->setToolTip({});
        } else {
            previewStatus->setText(
                !document ? QStringLiteral("Откройте учебную деталь bend-90-6mm.step.")
                : hasInteriorContours
                    ? QStringLiteral("В зоне сгиба обнаружен внутренний вырез. Выберите "
                                     "диагностику D слева. Отверстия и карманы внутри самой "
                                     "зоны пока не поддерживаются для движения.")
                : !supported ? QStringLiteral(
                                   "Выберите полный прямой сгиб. Остальная геометрия сохраняется "
                                   "и участвует в проверке; подрезанные торцы пока не поддержаны.")
                : entry && !included(entry->review)
                    ? QStringLiteral("Зона исключена или ожидает решения из сохранённого проекта. "
                                     "Откройте параметры выбранного сгиба и выберите участие.")
                : !matchingThickness
                    ? QStringLiteral("Ручная толщина отличается от геометрии. Для просмотра "
                                     "используйте толщину из STEP.")
                    : QStringLiteral("Параметры сгиба взяты из CAD. Выберите инструмент и нажмите "
                                     "«Проверить и показать гибку»."));
            setFrameText(QStringLiteral("Исходная CAD-форма"));
        }
        if (document && document->metadata().closedCutouts.status == ClosedCutoutStatus::Filled) {
            previewStatus->setText(
                QStringLiteral(
                    "ПРИБЛИЖЕНИЕ: отверстия видимы и перемещаются геометрически. "
                    "Опоры, пересечения и зазор проверяются по копии с заполненными вырезами. "
                    "Реальная опора у вырезов и деформация отверстий не проверены.\n\n") +
                previewStatus->text());
        }
        if (entry && entry->catalogTools) {
            previewStatus->setText(
                previewStatus->text() +
                QStringLiteral("\n\nИнструмент из каталога. Охват формы указан в условиях "
                               "проверки; допустимая нагрузка не проверена."));
        }
        singleBendSupported = supported;
        matchingBendThickness = matchingThickness;
        checkBlocker =
            !document ? QStringLiteral("Откройте STEP, чтобы проверить гибку.")
            : hasInteriorContours
                ? QStringLiteral("Вырез прерывает зону сгиба. Для этой геометрии движение "
                                 "пока не поддерживается. Выберите зону для подсветки.")
            : !supported
                ? QStringLiteral("Сейчас проверяется одна полная прямая зона с двумя "
                                 "плоскими полками. Маршрут нескольких гибов и подрезанные "
                                 "торцы пока не поддерживаются.")
            : !entry ? QStringLiteral("Не удалось привязать сгиб. Откройте параметры детали.")
            : !included(entry->review)
                ? QStringLiteral("Сгиб исключён или ожидает решения из проекта. "
                                 "Откройте параметры сгиба и выберите участие.")
            : !matchingThickness ? QStringLiteral("Ручная толщина отличается от модели. "
                                                  "В параметрах детали можно вернуть толщину CAD.")
            : !previewBuilder ? QStringLiteral("Проверка недоступна в этой сборке.")
            : !toolSelection->parameters()
                ? (toolSelection->openingIssue().isEmpty()
                       ? QStringLiteral("Выберите пуансон, матрицу и рабочий ручей.")
                       : toolSelection->openingIssue())
                : QString{};
        const bool busy = controller->isPreparingPreview() || controller->isPreparingRoute();
        const auto routeFailure = controller->bendRouteError();
        const bool failed = controller->bendPreviewError().has_value() || routeFailure ||
                            !previewDisplayError.isEmpty();
        refreshCheckResult();
        routePanel->reportWidget()->setVisible(multiple);
        checkScope->setText(
            (viewingRoute && controller->bendRoute()
                 ? QStringLiteral("Промежуточная 3D-форма по выбранному порядку. Извлечение и "
                                  "перенос между установками не проверены.\n")
                 : (document && document->metadata().bendAnalysis.bends.size() > 1
                        ? (preview
                               ? QStringLiteral("Выбранный гиб — как последняя операция; остальные "
                                                "гибы сохраняют форму STEP. Полный порядок не "
                                                "проверен.\n")
                               : QStringLiteral(
                                     "Проверка построит порядок всех гибов из заготовки. "
                                     "Лента станет пошаговым 3D-просмотром после "
                                     "успешного расчёта.\n"))
                        : QString{})) +
            (preview && preview->usesDrawingTools()
                 ? QStringLiteral("Пуансон: %1. Матрица: %2. ")
                       .arg(preview->usesDrawingPunch() ? QStringLiteral("по DXF")
                                                        : QStringLiteral("условный"),
                            preview->usesDrawingDie() ? QStringLiteral("по DXF")
                                                      : QStringLiteral("условная"))
             : entry && entry->catalogTools && entry->catalogTools->useDrawingGeometry
                 ? QStringLiteral("Выбрана форма DXF; результат её проверки ещё не подготовлен. ")
                 : QStringLiteral("Упрощённые рабочие профили. ")) +
            (preview && preview->machineModel() != core::MachineModel::None
                 ? QStringLiteral(
                       "Приближённые узлы учебного станка проверены по кадрам. "
                       "Непрерывный отвод пуансона проверен только против детали и матрицы. "
                       "Ход, усилие и пружинение не проверены.")
                 : QStringLiteral("Станок, держатели, усилие и пружинение не проверяются.")) +
            (preview && (preview->toolSegmentGaps()[0] > 0 || preview->toolSegmentGaps()[1] > 0)
                 ? QStringLiteral("\nЗазоры между секциями сохранены в 3D. Местная опора и "
                                  "качество гиба в промежутках не рассчитываются.")
                 : QString{}) +
            (document && document->metadata().closedCutouts.status == ClosedCutoutStatus::Filled
                 ? QStringLiteral(
                       "\nВырезы видимы; опоры и зазоры проверяются по заполненной копии. "
                       "Физика и реальная опора у вырезов не проверены.")
                 : QString{}));
        checkScope->setVisible(document != nullptr && !viewingDraft &&
                               !(viewingRoute && controller->bendRoute()));
        checkScope->setMinimumHeight(std::max(0, checkScope->heightForWidth(240)));
        checkBend->setEnabled(applyTools->isEnabled() && toolSelection->parameters().has_value());
        checkBend->setVisible(!preview || preview->unverifiedDraft());
        const auto checkTitle = !lastRouteAutomatic ? QStringLiteral("Проверить мой порядок")
                                : document && document->metadata().bendAnalysis.bends.size() > 1
                                    ? QStringLiteral("Проверить все гибы")
                                    : QStringLiteral("Проверить гибку");
        checkBend->setText(busy ? QStringLiteral("Проверяем…") : checkTitle);
        applyTools->setText(QStringLiteral("К гибке →"));
        applyTools->setEnabled(applyTools->isEnabled() && toolSelection->parameters().has_value());
        toolStatus->setText(busy ? QStringLiteral("Выполняется проверка…")
                            : !checkBlocker.isEmpty() ? checkBlocker
                            : toolDraftChanged
                                ? QStringLiteral("Выбор изменён. Нужна новая проверка.")
                                : QString{});
        toolStatus->setVisible(!toolStatus->text().isEmpty());
        checkAction = !supported                           ? CheckAction::Part
                      : !matchingThickness                 ? CheckAction::Thickness
                      : !entry || !included(entry->review) ? CheckAction::Bend
                                                           : CheckAction::Tools;
        if (const auto &failure = controller->bendPreviewError()) {
            if (failure->code == BendPreviewErrorCode::TargetMismatch)
                checkAction = CheckAction::Bend;
            else if (failure->code == BendPreviewErrorCode::UnsupportedGeometry ||
                     failure->code == BendPreviewErrorCode::BendZoneInteriorContours ||
                     failure->code == BendPreviewErrorCode::UnsupportedParameters)
                checkAction = CheckAction::Part;
        }
        if (routeFailure && routeFailure->code == BendRouteErrorCode::Unsupported)
            checkAction = CheckAction::Part;
        resolveCheck->setText(
            checkAction == CheckAction::Tools ? QStringLiteral("Изменить инструмент…")
            : checkAction == CheckAction::Thickness ? QStringLiteral("Уточнить толщину…")
            : checkAction == CheckAction::Bend ? QStringLiteral("Уточнить сгиб…")
                                               : QStringLiteral("Посмотреть деталь →"));
        resolveCheck->setVisible(document && !busy && (!preview || !stageSetupIssue.isEmpty()) &&
                                 (failed || !checkBlocker.isEmpty() || !stageSetupIssue.isEmpty()));
        resolveCheck->setEnabled(!controller->hasWork());
        checkSummary->setToolTip(routeFailure ? text(routeFailure->detail)
                                              : previewStatus->toolTip());
        if (draftAutoPlay && viewingDraft && preview && !controller->hasWork()) {
            draftAutoPlay = false;
            bendFrame->setValue(0);
            if (playBend->isEnabled()) {
                playBend->setText(QStringLiteral("Пауза"));
                playback->start();
            }
        }
        refreshWorkflow();
    }

    void showThicknessEvidence(bool updateInput) {
        bendList->setCurrentIndex({});
        const auto &session = controller->session();
        const auto &document = session.document();
        if (!document) {
            return;
        }
        std::vector<std::size_t> indices;
        const auto &analysis = document->metadata().thickness;
        if (pairs->currentIndex() >= 0) {
            const int selected = pairs->currentData().toInt();
            std::optional<double> value;
            if (selected < 0) {
                value = session.manualThicknessMm() ? session.manualThicknessMm()
                                                    : suggestThickness(analysis);
                if (value) {
                    for (const auto &pair : analysis.evidence) {
                        if (std::abs(pair.distanceMm - *value) <= analysis.toleranceMm) {
                            indices.push_back(pair.firstFace);
                            indices.push_back(pair.secondFace);
                        }
                    }
                }
            } else if (static_cast<std::size_t>(selected) < analysis.evidence.size()) {
                const auto &pair = analysis.evidence[static_cast<std::size_t>(selected)];
                value = pair.distanceMm;
                indices = {pair.firstFace, pair.secondFace};
            }
            if (updateInput && value) {
                thicknessInput->setValue(*value);
            }
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        viewer->highlightFaces(indices, session.revision());
    }
    void fillThicknessControls() {
        const auto &session = controller->session();
        const auto &document = session.document();
        const QSignalBlocker block(pairs);
        pairs->clear();
        thicknessInput->setValue(0.0);
        if (!document) {
            return;
        }
        const auto &analysis = document->metadata().thickness;
        const auto value =
            session.manualThicknessMm() ? session.manualThicknessMm() : suggestThickness(analysis);
        thicknessInput->setValue(value.value_or(0.0));
        const bool hasMatchingFaces =
            value &&
            std::any_of(analysis.evidence.begin(), analysis.evidence.end(), [&](const auto &pair) {
                return std::abs(pair.distanceMm - *value) <= analysis.toleranceMm;
            });
        if (hasMatchingFaces) {
            pairs->addItem(QStringLiteral("Поверхности для %1 мм · все пары").arg(number(*value)),
                           -1);
        }
        for (std::size_t i = 0; i < analysis.evidence.size(); ++i) {
            const auto &pair = analysis.evidence[i];
            pairs->addItem((pair.kind == ThicknessEvidenceKind::BendRadii
                                ? QStringLiteral("Радиусы гиба · грани %1 и %2 · %3 мм")
                                : QStringLiteral("Стенки · грани %1 и %2 · %3 мм"))
                               .arg(pair.firstFace)
                               .arg(pair.secondFace)
                               .arg(number(pair.distanceMm)),
                           static_cast<int>(i));
        }
        pairs->setCurrentIndex(hasMatchingFaces ? 0 : -1);
        if (!bendList->currentIndex().isValid())
            showThicknessEvidence(false);
    }
    void fillBends() {
        bendModel->clear();
        cutoutNotice->clear();
        showCutouts->hide();
        const auto &document = controller->session().document();
        if (!document) {
            bendSummary->setText(QStringLiteral("Сгибы: откройте деталь"));
            return;
        }
        const auto &analysis = document->metadata().bendAnalysis;
        const auto &cutouts = document->metadata().closedCutouts;
        if (cutouts.status == ClosedCutoutStatus::Filled) {
            cutoutNotice->setText(QStringLiteral("Вырезы найдены и подсвечены.\nВлияние на металл "
                                                 "и реальную опору не учитывается."));
        } else if (cutouts.status != ClosedCutoutStatus::None) {
            cutoutNotice->setText(QStringLiteral(
                "Не удалось надёжно заполнить замкнутые вырезы. "
                "Просмотр исходного CAD доступен; поддержка движения не подтверждена."));
        }
        showCutouts->setVisible(!cutouts.boundaryFaces.empty());
        switch (analysis.status) {
        case BendRecognitionStatus::Completed:
            bendSummary->setText(
                QStringLiteral("Найдено зон сгиба: %1").arg(analysis.bends.size()));
            if (analysis.unrecognizedCylindricalFaces > 0) {
                bendSummary->setText(bendSummary->text() +
                                     QStringLiteral("\nДругих цилиндрических граней: %1.")
                                         .arg(analysis.unrecognizedCylindricalFaces));
            }
            break;
        case BendRecognitionStatus::LimitReached:
            bendSummary->setText(QStringLiteral("Анализ сгибов: превышен лимит сложности."));
            break;
        case BendRecognitionStatus::Failed:
            bendSummary->setText(
                QStringLiteral("Анализ сгибов завершился ошибкой. Просмотр доступен."));
            break;
        case BendRecognitionStatus::Unsupported:
            bendSummary->setText(
                QStringLiteral("Допуски геометрии не поддерживаются анализом сгибов."));
            break;
        case BendRecognitionStatus::NotAnalyzed:
            bendSummary->setText(QStringLiteral("Сгибы не анализировались."));
            break;
        }
        bendSummary->setToolTip(QStringLiteral("Локальное распознавание прямых цилиндрических зон. "
                                               "Полнота распознавания не подтверждена."));
        if (!controller->session().bendBindings().unresolved.empty()) {
            bendSummary->setText(
                bendSummary->text() +
                QStringLiteral(
                    "\nНе привязано сохранённых записей: %1. Подробности на вкладке «Сгиб».")
                    .arg(controller->session().bendBindings().unresolved.size()));
        }
        if (!analysis.interiorContourDiagnostics.empty() &&
            cutouts.status != ClosedCutoutStatus::Filled) {
            bendSummary->setText(
                bendSummary->text() +
                QStringLiteral(
                    "\nЗон с внутренними вырезами: %1. Выберите D для просмотра причины.")
                    .arg(analysis.interiorContourDiagnostics.size()));
        }
        for (std::size_t i = 0; i < analysis.bends.size(); ++i) {
            const auto &bend = analysis.bends[i];
            auto *item =
                new QStandardItem(QStringLiteral("B%1 · %2° · R%3 · L%4 мм")
                                      .arg(i + 1, 2, 10, QLatin1Char('0'))
                                      .arg(number(bend.angleRadians * 180.0 / std::numbers::pi),
                                           number(bend.innerRadiusMm), number(bend.lengthMm)));
            if (cutouts.status == ClosedCutoutStatus::Filled) {
                item->setText(item->text() + QStringLiteral(" · приближение"));
            }
            if (bend.trimmedEnds) {
                item->setText(item->text() + QStringLiteral(" · подрезка"));
            }
            item->setToolTip(item->text());
            item->setData(item->text(), Qt::UserRole);
            item->setEditable(false);
            bendModel->appendRow(item);
        }
        for (std::size_t i = 0; cutouts.status != ClosedCutoutStatus::Filled &&
                                i < analysis.interiorContourDiagnostics.size();
             ++i) {
            auto *item = new QStandardItem(
                QStringLiteral("D%1 · Контур в зоне сгиба").arg(i + 1, 2, 10, QLatin1Char('0')));
            item->setData(item->text(), Qt::UserRole);
            item->setEditable(false);
            bendModel->appendRow(item);
        }
    }
    void updateBendReviews() {
        const auto &session = controller->session();
        for (int row = 0; row < bendModel->rowCount(); ++row) {
            auto *item = bendModel->item(row);
            const auto *entry = session.bend(static_cast<std::size_t>(row));
            const auto &document = session.document();
            const bool diagnosticRow =
                document &&
                static_cast<std::size_t>(row) >= document->metadata().bendAnalysis.bends.size();
            const auto status = diagnosticRow ? QStringLiteral("Движение недоступно")
                                : entry ? reviewText(entry->review)
                                        : QStringLiteral("Нет однозначной привязки");
            item->setText(item->data(Qt::UserRole).toString() + QLatin1Char('\n') + status);
            item->setToolTip(item->text());
        }
        QStringList unresolved;
        for (const auto index : session.bendBindings().unresolved) {
            const auto &entry = session.bends()[index];
            auto line = text(entry.id) + QStringLiteral(" · ") + reviewText(entry.review);
            if (entry.target) {
                line += QStringLiteral(" · задание %1° / R%2 мм")
                            .arg(number(entry.target->angleRadians * 180.0 / std::numbers::pi),
                                 number(entry.target->innerRadiusMm));
            }
            unresolved.append(line);
        }
        bendBindingWarning->setVisible(!unresolved.isEmpty());
        bendBindingWarning->setText(
            QStringLiteral("Не удалось однозначно привязать сохранённые записи. Они сохранены, "
                           "но не применяются к найденным зонам:\n%1")
                .arg(unresolved.join(QLatin1Char('\n'))));
    }
    void updateTargetButtons() {
        const auto row = bendList->currentIndex().row();
        const auto *entry =
            row < 0 ? nullptr : controller->session().bend(static_cast<std::size_t>(row));
        const bool editable = entry && included(entry->review) && !controller->hasWork();
        targetGroup->setEnabled(editable);
        applyBendTarget->setEnabled(
            editable && core::validBendTarget({targetAngle->value() * std::numbers::pi / 180.0,
                                               targetRadius->value()}));
        resetBendTarget->setEnabled(editable && entry->target.has_value());
    }
    void showSelectedBend(bool highlight) {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        bendReview->setEnabled(entry && !controller->hasWork());
        updateTargetButtons();
        if (!document || row < 0 ||
            static_cast<std::size_t>(row) >= document->metadata().bendAnalysis.bends.size()) {
            bendProperties->setText(QStringLiteral(
                "Выберите найденный сгиб слева, чтобы посмотреть параметры и поверхности."));
            bendTargetStatus->setText(QStringLiteral("Задание сгиба не выбрано."));
            const QSignalBlocker block(bendReview);
            bendReview->setCurrentIndex(-1);
            targetAngle->setValue(90.0);
            targetRadius->setValue(0.0);
            shownBendEditRevision.reset();
            if (document && row >= 0 &&
                static_cast<std::size_t>(row) >= document->metadata().bendAnalysis.bends.size()) {
                const auto &analysis = document->metadata().bendAnalysis;
                const auto diagnosticIndex = static_cast<std::size_t>(row) - analysis.bends.size();
                if (diagnosticIndex < analysis.interiorContourDiagnostics.size()) {
                    const auto &diagnostic = analysis.interiorContourDiagnostics[diagnosticIndex];
                    const auto &candidate = diagnostic.candidate;
                    bendProperties->setText(
                        QStringLiteral(
                            "D%1 · Внутренний вырез в зоне сгиба\n\n"
                            "На цилиндрической поверхности обнаружены замкнутые внутренние "
                            "контуры: "
                            "отверстие или карман прерывает предполагаемую зону сгиба.\n\n"
                            "Параметры внешней границы по CAD:\nПоворот: %2°\nВнутренний радиус: "
                            "%3 мм\n"
                            "Расстояние между цилиндрами: %4 мм\nГабарит вдоль оси: %5 мм\n\n"
                            "Оранжевая заливка — предполагаемая зона; красные поверхности — "
                            "границы выреза.\n\n"
                            "Подтверждение и движение недоступны. Деформация отверстия, усилие и "
                            "риск трещин не рассчитываются. Это ограничение текущего алгоритма; "
                            "оно не доказывает невозможность изготовления.")
                            .arg(diagnosticIndex + 1, 2, 10, QLatin1Char('0'))
                            .arg(number(candidate.angleRadians * 180.0 / std::numbers::pi),
                                 number(candidate.innerRadiusMm), number(candidate.thicknessMm),
                                 number(candidate.lengthMm)));
                    bendTargetStatus->setText(QStringLiteral(
                        "Диагностика геометрии. Задание операции здесь не создаётся."));
                    if (highlight && !controller->hasWork()) {
                        viewer->highlightBendDiagnostic(diagnostic, session.revision());
                    }
                }
            }
            return;
        }
        const auto &bend = document->metadata().bendAnalysis.bends[static_cast<std::size_t>(row)];
        const auto coordinate = [&](double value) {
            return number(std::abs(value) <= document->metadata().bendAnalysis.lengthToleranceMm
                              ? 0.0
                              : value);
        };
        QString description =
            QStringLiteral(
                "B%1 · измерено по CAD\nУгол поворота полок: %2°\nВнутренний радиус: %3 мм\n"
                "Толщина зоны: %4 мм\nДлина сгиба: %5 мм\n\nОсь, мм:\n(%6; %7; %8) →\n(%9; %10; "
                "%11)\n\n"
                "Это неизменяемые измерения загруженной формы.")
                .arg(row + 1, 2, 10, QLatin1Char('0'))
                .arg(number(bend.angleRadians * 180.0 / std::numbers::pi),
                     number(bend.innerRadiusMm), number(bend.thicknessMm), number(bend.lengthMm),
                     coordinate(bend.axisStart.x), coordinate(bend.axisStart.y),
                     coordinate(bend.axisStart.z), coordinate(bend.axisEnd.x),
                     coordinate(bend.axisEnd.y), coordinate(bend.axisEnd.z));
        description += QStringLiteral("\n\nОранжевая заливка и светлая обводка — зона сгиба. "
                                      "Тонкая голубая обводка — прилегающие полки.");
        if (bend.trimmedEnds) {
            description.replace(QStringLiteral("Длина сгиба:"),
                                QStringLiteral("Габарит вдоль оси:"));
            description +=
                QStringLiteral("\n\nТорцы подрезаны. Прямой участок без подрезки — не менее %1 мм.")
                    .arg(number(bend.minimumStraightLengthMm));
        }
        if (session.manualThicknessMm() &&
            std::abs(*session.manualThicknessMm() - bend.thicknessMm) >
                document->metadata().bendAnalysis.lengthToleranceMm) {
            description += QStringLiteral("\nРучная толщина отличается от толщины этой зоны.");
        }
        if (document->metadata().closedCutouts.status == ClosedCutoutStatus::Filled) {
            description += QStringLiteral("\n\nКрасные грани — распознанные замкнутые вырезы. "
                                          "В анимации вырезы остаются видимыми. Для проверок они "
                                          "заполнены в отдельной копии. "
                                          "Исходная CAD-форма не изменяется. Влияние вырезов на "
                                          "материал и реальную опору не оценивается.");
        }
        bendProperties->setText(description);
        if (highlight || shownBendEditRevision != session.editRevision() || shownTargetRow != row) {
            shownBendEditRevision = session.editRevision();
            shownTargetRow = row;
            const QSignalBlocker block(bendReview);
            bendReview->setCurrentIndex(entry ? static_cast<int>(entry->review) : -1);
            const auto target = entry && entry->target
                                    ? *entry->target
                                    : core::BendTarget{bend.angleRadians, bend.innerRadiusMm};
            targetAngle->setValue(target.angleRadians * 180.0 / std::numbers::pi);
            targetRadius->setValue(target.innerRadiusMm);
        }
        bendTargetStatus->setText(
            !entry ? QStringLiteral(
                         "Сохранённые записи не привязаны к этой зоне. Редактирование недоступно.")
            : entry->target
                ? QStringLiteral("Задание применено: %1° · R%2 мм%3")
                      .arg(number(entry->target->angleRadians * 180.0 / std::numbers::pi),
                           number(entry->target->innerRadiusMm),
                           included(entry->review) ? QString{} : QStringLiteral(" · не активно"))
                : QStringLiteral("Используются угол и радиус из CAD. Поля позволяют задать "
                                 "исключение вручную."));
        updateTargetButtons();
        if (highlight && !controller->hasWork()) {
            if (document->metadata().closedCutouts.status == ClosedCutoutStatus::Filled) {
                viewer->highlightBendDiagnostic(
                    {bend, document->metadata().closedCutouts.boundaryFaces}, session.revision());
            } else {
                viewer->highlightBend(bend, session.revision());
            }
        }
    }
    bool canDiscard() {
        if (!controller->session().modified()) {
            return true;
        }
        return QMessageBox::warning(
                   window, QStringLiteral("Несохранённые изменения"),
                   QStringLiteral(
                       "В проекте есть несохранённые параметры. Продолжить без сохранения?"),
                   QMessageBox::Discard | QMessageBox::Cancel,
                   QMessageBox::Cancel) == QMessageBox::Discard;
    }
    void advanceUpdate() {
        if (!updatePending || !updateInstaller)
            return;
        if (controller->hasWork()) {
            if (updateDialog)
                updateDialog->exitStatus(
                    QStringLiteral("Ждём завершения текущей операции. Перезапуск можно отменить."),
                    true);
            return;
        }
        const auto stop = [&](const QString &message) {
            updatePending = false;
            updateSaving = false;
            if (updateDialog)
                updateDialog->exitStatus(message, false);
        };
        if (updateSaving) {
            updateSaving = false;
            if (!controller->saveError().isEmpty() || controller->session().modified()) {
                stop(QStringLiteral(
                    "Проект не сохранён. Перезапуск отменён; изменения остались в программе."));
                return;
            }
        }
        if (controller->session().modified() && !updateDiscard) {
            const auto choice = QMessageBox::question(
                window, QStringLiteral("Сохранить проект перед обновлением?"),
                QStringLiteral("В проекте есть несохранённые параметры."),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (choice == QMessageBox::Cancel) {
                stop(QStringLiteral(
                    "Перезапуск отменён. Проверенный пакет сохранён до закрытия программы."));
                return;
            }
            if (choice == QMessageBox::Save) {
                updateSaving = true;
                save(false);
                if (!controller->hasWork())
                    stop(QStringLiteral("Сохранение отменено. Программа остаётся открытой."));
                return;
            }
            updateDiscard = true;
        }
        QString reopen = displayPath(controller->session().projectPath());
        if (reopen.isEmpty() && controller->session().document())
            reopen = displayPath(controller->session().document()->metadata().sourcePath);
        auto settings = preferences();
        settings->sync();
        if (settings->status() != QSettings::NoError) {
            stop(QStringLiteral("Не удалось сохранить настройки. Перезапуск отменён; проверьте "
                                "доступ к папке программы."));
            return;
        }
        const auto installError = updateInstaller(reopen);
        if (!installError.isEmpty()) {
            stop(installError);
            return;
        }
        updatePending = false;
        updateClosing = true;
        if (updateDialog)
            updateDialog->close();
        window->close();
    }
    void save(bool choosePath) {
        if (toolDraftChanged && !commitStageTools()) {
            propertyTabs->setCurrentIndex(3);
            refreshPreview();
            return;
        }
        auto path = displayPath(controller->session().projectPath());
        if (choosePath || path.isEmpty()) {
            QFileDialog dialog(window, QStringLiteral("Сохранить проект"), path,
                               QStringLiteral("Проект симулятора (*.bend)"));
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setDefaultSuffix(QStringLiteral("bend"));
            if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
                return;
            }
            path = dialog.selectedFiles().first();
            if (!path.endsWith(QStringLiteral(".bend"), Qt::CaseInsensitive)) {
                QMessageBox::warning(window, QStringLiteral("Формат проекта"),
                                     QStringLiteral("Имя проекта должно заканчиваться на .bend."));
                return;
            }
        }
        controller->saveProject(nativePath(path), writer);
    }
    MainWindow *window;
    RoutePanel *routePanel = nullptr;
    QComboBox *toolAssignmentScope = nullptr;
    QPushButton *assignBendTool = nullptr;
    QLabel *toolAssignmentStatus = nullptr;
    std::uint64_t toolScopeRevision = 0;
    std::string toolContextId;
    bool viewingRoute = false;
    bool viewingDraft = false, draftAutoPlay = false;
    std::weak_ptr<const application::RouteDraft> shortToolDiagnosticDraft;
    QToolButton *shortToolDiagnostic = nullptr;
    bool lastRouteAutomatic = true;
    bool checkedToolJob = false, checkedToolCancelled = false, checkedToolManualOverride = false;
    std::uint64_t checkedToolDocument = 0, checkedToolEdit = 0;
    std::uint64_t initialToolSuggestion = 0;
    std::vector<application::RouteToolVariant> checkedToolVariants;
    ToolSearchDialog *checkedToolDialog = nullptr;
    viewer::ViewerWidget *viewer = nullptr;
    QStackedWidget *workspaceStack = nullptr;
    ToolWorkspace *toolWorkspace = nullptr;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> setupAttempt;
    std::optional<std::uint64_t> pendingBendStage;
    QString stageSetupIssue;
    std::optional<std::size_t> stageToolIssueIndex;
    ImportController *controller = nullptr;
    QListWidget *project = nullptr;
    QListView *bendList = nullptr;
    QStandardItemModel *bendModel = nullptr;
    QLabel *bendSummary = nullptr;
    QLabel *bendProperties = nullptr;
    QLabel *bendBindingWarning = nullptr;
    QComboBox *bendReview = nullptr;
    QGroupBox *targetGroup = nullptr;
    QLabel *bendTargetStatus = nullptr;
    QDoubleSpinBox *targetAngle = nullptr;
    QDoubleSpinBox *targetRadius = nullptr;
    QPushButton *applyBendTarget = nullptr;
    QPushButton *resetBendTarget = nullptr;
    std::shared_ptr<const SingleBendPreviewBuilder> previewBuilder;
    std::shared_ptr<const BendingDrawingBuilder> drawingBuilder;
    std::shared_ptr<const application::FlatPatternBuilder> flatBuilder;
    application::FlatPatternPtr displayedFlat;
    bool wantFlat = false;
    QLabel *partInputs = nullptr;
    QPushButton *editInputs = nullptr;
    QPointer<PartInputDialog> partInputDialog;
    bool automaticInputPrompt = true;
    std::uint64_t inputPromptSerial = 0;
    QAction *flatAction = nullptr;
    QPushButton *flatButton = nullptr, *checkFlatButton = nullptr, *flatSource = nullptr,
                *checkFlatSource = nullptr;
    QLabel *flatStatus = nullptr, *checkFlatStatus = nullptr;
    QAction *drawingAction = nullptr;
    QPointer<DrawingDialog> drawingDialog;
    QPointer<update::UpdateClient> updates;
    QPointer<UpdateDialog> updateDialog;
    QAction *updatesAction = nullptr;
    bool updatePending = false, updateSaving = false, updateDiscard = false, updateClosing = false;
    std::function<QString(const QString &)> updateInstaller;
    std::optional<std::size_t> drawingBase;
    std::uint64_t drawingBaseRevision = 0;
    application::BendPreviewPtr displayedPreview;
    std::size_t displayedPreviewFrame = 0;
    std::uint64_t displayedPreviewRevision = 0;
    QString previewDisplayError;
    QString operationFailureSummary;
    QComboBox *fixedSide = nullptr;
    QPushButton *preparePreview = nullptr;
    QLabel *previewStatus = nullptr;
    QLabel *checkSummary = nullptr;
    QToolButton *kinematicsToggle = nullptr;
    QWidget *kinematicsPanel = nullptr;
    ToolSelectionWidget *toolSelection = nullptr;
    QPushButton *checkBend = nullptr;
    std::array<ProcessStepButton *, 3> processSteps{};
    QPushButton *partPrimary = nullptr;
    QCheckBox *sheetSides = nullptr;
    QPushButton *changePartTools = nullptr;
    QPushButton *editBend = nullptr;
    QLabel *partName = nullptr;
    QLabel *partToolSummary = nullptr;
    QLabel *partReadiness = nullptr;
    QLabel *checkScope = nullptr;
    QPushButton *resolveCheck = nullptr;
    QString checkBlocker;
    bool toolDraftChanged = false;
    bool toolDraftPunchSelected = false, toolDraftDieSelected = false;
    bool singleBendSupported = false;
    bool matchingBendThickness = true;
    enum class CheckAction { Part, Tools, Bend, Thickness };
    CheckAction checkAction = CheckAction::Part;
    bool viewingSource = false;
    application::BendPreviewPtr positionedPreview;
    struct PreviewReplacement {
        application::DocumentPtr document;
        std::uint64_t revision;
        application::BendPreviewPtr previous;
        std::size_t frame;
        bool preserveFrame;
    };
    std::optional<PreviewReplacement> previewReplacement;
    QWidget *measurements = nullptr;
    QToolButton *measurementsToggle = nullptr;
    std::optional<core::ToolAssignment> shownAssignment;
    QPushButton *applyTools = nullptr;
    QComboBox *toolMode = nullptr;
    QPushButton *clearTools = nullptr;
    QLabel *toolStatus = nullptr;
    std::uint64_t toolsDocumentRevision = 0;
    std::string toolsEntryId;
    std::optional<core::PressTools> shownTools;
    QSlider *bendFrame = nullptr;
    QPushButton *playBend = nullptr;
    QToolButton *reversePunchButton = nullptr;
    QToolButton *reverseWorkpieceButton = nullptr;
    std::array<QIcon, 2> workpieceOrientationIcons;
    std::array<QIcon, 2> punchOrientationIcons;
    std::optional<core::ToolPreset> facingPunch;
    std::optional<bool> facingPreference;
    QWidget *playbackSpeedControls = nullptr;
    QSlider *playbackSpeed = nullptr;
    QLabel *playbackSpeedValue = nullptr;
    QPushButton *showKinematics = nullptr;
    QLabel *setupPlaybackHint = nullptr;
    QStackedWidget *previewControls = nullptr;
    QPushButton *returnToCad = nullptr;
    QLabel *frameLabel = nullptr;
    QTimer *playback = nullptr;
    QWidget *playbackControls = nullptr;
    QWidget *operationOverlay = nullptr;
    BendMessagePanel *bendMessages = nullptr;
    QLabel *operationSummary = nullptr;
    std::optional<std::uint64_t> shownBendEditRevision;
    int shownTargetRow = -1;
    QTabWidget *propertyTabs = nullptr;
    std::optional<std::uint64_t> shownBendRevision;
    QLabel *properties = nullptr;
    QLabel *selection = nullptr;
    QLabel *thickness = nullptr;
    QLabel *projectState = nullptr;
    QDoubleSpinBox *thicknessInput = nullptr;
    QComboBox *pairs = nullptr;
    QPushButton *applyThickness = nullptr;
    QPushButton *automaticThickness = nullptr;
    QPushButton *showPair = nullptr;
    QAction *saveAction = nullptr;
    QAction *saveAsAction = nullptr;
    std::shared_ptr<const ProjectWriter> writer;
    bool showingFailure = false;
    QLabel *error = nullptr;
    QPlainTextEdit *diagnostics = nullptr;
    QLabel *cutoutNotice = nullptr;
    QPushButton *showCutouts = nullptr;
    QAction *closeAction = nullptr;
    QAction *cancelAction = nullptr;
    QProgressBar *progress = nullptr;
    QString title;
    std::optional<std::uint64_t> shownRevision;
    std::optional<std::uint64_t> shownThicknessRevision;
    std::optional<double> shownManualThickness;
};

MainWindow::MainWindow(RuntimeInfo runtime,
                       std::shared_ptr<const application::DocumentImporter> importer,
                       std::shared_ptr<const application::ProjectWriter> writer,
                       std::shared_ptr<const application::SingleBendPreviewBuilder> previewBuilder,
                       core::ToolCatalog catalog)
    : impl_(std::make_unique<Impl>(this)) {
    impl_->writer = std::move(writer);
    impl_->previewBuilder = std::move(previewBuilder);
    setObjectName(QStringLiteral("mainWindow"));
    setAcceptDrops(true);
    setStyleSheet(QStringLiteral(
        "QPushButton#partPrimaryAction,QPushButton#applyToolSetup,"
        "QPushButton#checkBend,QPushButton#playBend{background:#087F83;color:white;"
        "border:1px solid #087F83;border-radius:4px;padding:8px 12px;}"
        "QPushButton#partPrimaryAction:disabled,QPushButton#applyToolSetup:disabled,"
        "QPushButton#checkBend:disabled,QPushButton#playBend:disabled{background:#E8ECEF;"
        "color:#526171;border-color:#DCE3E9;}"
        "QPushButton:focus{outline:2px solid #087F83;}"));
    impl_->title =
        QStringLiteral("3D Bending Simulator · %1").arg(text(runtime.applicationVersion));
    resize(1440, 900);
    impl_->viewer = new viewer::ViewerWidget(this);
    impl_->viewer->setToolSegmentColors(toolSegmentColors());
    auto *workspace = new QWidget(this);
    auto *workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    impl_->routePanel = new RoutePanel(workspace);
    impl_->routePanel->hide();
    impl_->workspaceStack = new QStackedWidget(workspace);
    impl_->workspaceStack->setObjectName("stageWorkspace");
    impl_->workspaceStack->addWidget(impl_->viewer);
    impl_->toolWorkspace = new ToolWorkspace(impl_->workspaceStack);
    impl_->workspaceStack->addWidget(impl_->toolWorkspace);
    workspaceLayout->addWidget(impl_->workspaceStack, 1);
    setCentralWidget(workspace);
    impl_->controller = new ImportController(
        std::move(importer),
        [this](application::DocumentPtr document, std::uint64_t revision) {
            if (!document) {
                impl_->viewer->clearDocument(revision);
                return QString{};
            }
            return impl_->viewer->presentDocument(std::move(document), revision);
        },
        this);

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("Файл"));
    auto *open = fileMenu->addAction(QStringLiteral("Открыть STEP или проект…"));
    open->setObjectName(QStringLiteral("openStepAction"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this] {
        QString folder;
        if (const auto &document = impl_->controller->session().document()) {
            folder = displayPath(document->metadata().sourcePath.parent_path());
        } else {
            const auto installed =
                QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("../share/3d-bending-simulator/examples"));
            const auto local = QDir::current().filePath(QStringLiteral("examples"));
            folder = QDir(installed).exists() ? installed
                     : QDir(local).exists()   ? local
                                              : QString{};
        }
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Открыть деталь или проект"), folder,
            QStringLiteral(
                "Деталь и проект (*.step *.stp *.STEP *.STP *.bend *.BEND);;Все файлы (*)"));
        if (!path.isEmpty()) {
            openStep(path);
        }
    });
    impl_->saveAction = fileMenu->addAction(QStringLiteral("Сохранить проект"));
    impl_->saveAction->setObjectName(QStringLiteral("saveProjectAction"));
    impl_->saveAction->setShortcut(QKeySequence::Save);
    connect(impl_->saveAction, &QAction::triggered, this, [this] { impl_->save(false); });
    impl_->saveAsAction = fileMenu->addAction(QStringLiteral("Сохранить проект как…"));
    impl_->saveAsAction->setObjectName(QStringLiteral("saveProjectAsAction"));
    impl_->saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(impl_->saveAsAction, &QAction::triggered, this, [this] { impl_->save(true); });
    impl_->cancelAction = fileMenu->addAction(QStringLiteral("Отменить операцию"));
    impl_->cancelAction->setObjectName(QStringLiteral("cancelImportAction"));
    impl_->cancelAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(impl_->cancelAction, &QAction::triggered, impl_->controller, &ImportController::cancel);
    impl_->closeAction = fileMenu->addAction(QStringLiteral("Закрыть деталь"));
    impl_->closeAction->setObjectName(QStringLiteral("closeDocumentAction"));
    impl_->closeAction->setIconText(QStringLiteral("Закрыть"));
    impl_->closeAction->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    impl_->closeAction->setToolTip(QStringLiteral("Закрыть деталь · Ctrl+W"));
    impl_->closeAction->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+W")), QKeySequence(QStringLiteral("Ctrl+F4"))});
    connect(impl_->closeAction, &QAction::triggered, this, [this] {
        if (impl_->canDiscard()) {
            impl_->controller->closeDocument();
        }
    });
    impl_->drawingAction = fileMenu->addAction(QStringLiteral("Лист гибки / PDF…"));
    impl_->drawingAction->setObjectName(QStringLiteral("bendingDrawingAction"));
    impl_->drawingAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    impl_->drawingAction->setEnabled(false);
    connect(impl_->drawingAction, &QAction::triggered, this, [this] {
        if (impl_->drawingDialog) {
            if (impl_->drawingDialog->isMinimized())
                impl_->drawingDialog->showNormal();
            impl_->drawingDialog->show();
            impl_->drawingDialog->raise();
            impl_->drawingDialog->activateWindow();
            return;
        }
        const auto &session = impl_->controller->session();
        if (!impl_->drawingBuilder || !session.document() || impl_->controller->hasWork())
            return;
        impl_->stopPlayback();
        auto *dialog = new DrawingDialog(
            *impl_->controller, impl_->drawingBuilder,
            impl_->drawingBaseRevision == session.revision() ? impl_->drawingBase : std::nullopt,
            this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        impl_->drawingDialog = dialog;
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            if (impl_->drawingDialog == dialog)
                impl_->drawingDialog.clear();
            impl_->refresh();
        });
        dialog->show();
    });
    fileMenu->addSeparator();
    auto *exit = fileMenu->addAction(QStringLiteral("Выход"));
    exit->setShortcut(QKeySequence::Quit);
    connect(exit, &QAction::triggered, this, &QWidget::close);
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("Вид"));

    auto *toolbar = addToolBar(QStringLiteral("Подготовка"));
    toolbar->setObjectName(QStringLiteral("workflowToolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(open);
    auto *quickClose = new QToolButton(toolbar);
    quickClose->setObjectName(QStringLiteral("quickClosePart"));
    quickClose->setDefaultAction(impl_->closeAction);
    quickClose->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    quickClose->setToolTip(QStringLiteral("Закрыть деталь · Ctrl+W"));
    quickClose->setAccessibleName(QStringLiteral("Закрыть текущую деталь"));
    toolbar->addWidget(quickClose);
    toolbar->addAction(impl_->saveAction);
    toolbar->addAction(impl_->cancelAction);
    auto *toolSetupAction = viewMenu->addAction(QStringLiteral("Инструмент"));
    toolSetupAction->setObjectName(QStringLiteral("toolSetupAction"));
    connect(toolSetupAction, &QAction::triggered, this, [this] {
        impl_->propertyTabs->setCurrentIndex(3);
        impl_->propertyTabs->parentWidget()->show();
    });
    if (auto *navigation = impl_->viewer->navigationMenu())
        viewMenu->addMenu(navigation);

    const auto addDock = [&](const QString &name, const QString &title, QWidget *widget,
                             Qt::DockWidgetArea area) {
        auto *dock = new QDockWidget(title, this);
        dock->setObjectName(name);
        dock->setWidget(widget);
        dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        dock->setTitleBarWidget(new QWidget(dock));
        addDockWidget(area, dock);
    };
    const auto taskPage = [](QWidget *content, QPushButton *primary, QWidget *parent) {
        auto *page = new QWidget(parent);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(0);
        auto *scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        pageLayout->addWidget(scroll, 1);
        content->layout()->removeWidget(primary);
        auto *footer = new QWidget(page);
        auto *footerLayout = new QVBoxLayout(footer);
        footerLayout->setContentsMargins(18, 12, 18, 18);
        primary->setMinimumHeight(44);
        footerLayout->addWidget(primary);
        pageLayout->addWidget(footer);
        page->setMinimumWidth(320);
        return page;
    };
    impl_->project = new QListWidget(this);
    impl_->project->setMinimumWidth(230);
    impl_->project->setMaximumHeight(100);
    impl_->project->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *leftPanel = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->addWidget(impl_->project);
    impl_->bendSummary = new QLabel(leftPanel);
    impl_->bendSummary->setObjectName(QStringLiteral("bendSummary"));
    impl_->bendSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->bendSummary->setWordWrap(true);
    impl_->bendSummary->setTextFormat(Qt::PlainText);
    leftLayout->addWidget(impl_->bendSummary);
    impl_->cutoutNotice = new QLabel(leftPanel);
    impl_->cutoutNotice->setObjectName(QStringLiteral("cutoutNotice"));
    impl_->cutoutNotice->setWordWrap(true);
    impl_->cutoutNotice->setTextFormat(Qt::PlainText);
    impl_->cutoutNotice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->cutoutNotice->setStyleSheet(QStringLiteral("color:#85530C;"));
    leftLayout->addWidget(impl_->cutoutNotice);
    impl_->showCutouts = new QPushButton(QStringLiteral("Подсветка вырезов"), leftPanel);
    impl_->showCutouts->setObjectName(QStringLiteral("showIgnoredCutouts"));
    leftLayout->addWidget(impl_->showCutouts);
    impl_->showCutouts->setCheckable(true);
    impl_->showCutouts->setChecked(true);
    connect(impl_->showCutouts, &QPushButton::toggled, impl_->viewer,
            &viewer::ViewerWidget::setCutoutsVisible);
    impl_->bendList = new QListView(leftPanel);
    impl_->bendList->setObjectName(QStringLiteral("bendList"));
    impl_->bendList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    impl_->bendList->setAccessibleName(QStringLiteral("Распознанные зоны сгибов"));
    impl_->bendModel = new QStandardItemModel(impl_->bendList);
    impl_->bendList->setModel(impl_->bendModel);
    leftLayout->addWidget(impl_->bendList);
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(18, 18, 18, 18);
    auto *partTitle = new QLabel(QStringLiteral("Деталь"), panel);
    partTitle->setStyleSheet(QStringLiteral("font-size:18px;font-weight:500;"));
    layout->addWidget(partTitle);
    impl_->partName = new QLabel(panel);
    impl_->partName->setObjectName(QStringLiteral("partName"));
    impl_->partName->setWordWrap(true);
    impl_->partName->setTextFormat(Qt::PlainText);
    layout->addWidget(impl_->partName);
    const auto label = [&](const QString &name) {
        auto *result = new QLabel(panel);
        result->setObjectName(name);
        result->setWordWrap(true);
        result->setTextFormat(Qt::PlainText);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(result);
        return result;
    };
    impl_->properties = label(QStringLiteral("documentProperties"));
    impl_->projectState = label(QStringLiteral("projectState"));
    impl_->thickness = label(QStringLiteral("thicknessProperties"));
    impl_->partInputs = label(QStringLiteral("partInputsSummary"));
    impl_->editInputs = new QPushButton(QStringLiteral("Изменить…"), panel);
    impl_->editInputs->setObjectName(QStringLiteral("editPartInputs"));
    impl_->editInputs->setAccessibleName(QStringLiteral("Изменить материал и толщину детали"));
    layout->addWidget(impl_->editInputs);
    connect(impl_->editInputs, &QPushButton::clicked, this, [this] { impl_->editPartInputs(); });
    impl_->flatAction = new QAction(QStringLiteral("Подготовить развёртку"), this);
    impl_->flatAction->setObjectName(QStringLiteral("flatPatternAction"));
    impl_->flatAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+U")));
    addAction(impl_->flatAction);
    connect(impl_->flatAction, &QAction::triggered, this, [this] { impl_->showFlat(); });
    if (auto *menu = impl_->viewer->navigationMenu()) {
        menu->addSeparator();
        menu->addAction(impl_->flatAction);
        auto *source = menu->addAction(QStringLiteral("Показать деталь (STEP)"));
        connect(source, &QAction::triggered, this, [this] {
            impl_->showSource();
            impl_->refreshFlat();
        });
    }
    impl_->flatButton = new QPushButton(panel);
    impl_->flatButton->setObjectName(QStringLiteral("showFlatPattern"));
    connect(impl_->flatButton, &QPushButton::clicked, impl_->flatAction, &QAction::trigger);
    layout->addWidget(impl_->flatButton);
    impl_->flatSource = new QPushButton(QStringLiteral("Показать деталь"), panel);
    connect(impl_->flatSource, &QPushButton::clicked, this, [this] {
        impl_->showSource();
        impl_->refreshFlat();
    });
    layout->addWidget(impl_->flatSource);
    impl_->flatStatus = label(QStringLiteral("flatPatternStatus"));
    leftLayout->removeWidget(impl_->project);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    impl_->bendList->setMaximumHeight(180);
    layout->addWidget(leftPanel);
    impl_->partToolSummary = label(QStringLiteral("partToolSummary"));
    impl_->partReadiness = label(QStringLiteral("partReadiness"));
    impl_->thicknessInput = new QDoubleSpinBox(panel);
    impl_->thicknessInput->setObjectName(QStringLiteral("thicknessInput"));
    impl_->thicknessInput->setAccessibleName(QStringLiteral("Толщина листа в миллиметрах"));
    impl_->thicknessInput->setDecimals(6);
    impl_->thicknessInput->setRange(0.0, 1000000.0);
    impl_->thicknessInput->setSpecialValueText(QStringLiteral("Не определена"));
    impl_->thicknessInput->setSuffix(QStringLiteral(" мм"));
    layout->addWidget(impl_->thicknessInput);
    impl_->applyThickness = new QPushButton(QStringLiteral("Применить толщину вручную"), panel);
    impl_->applyThickness->setObjectName(QStringLiteral("applyThickness"));
    layout->addWidget(impl_->applyThickness);
    connect(impl_->thicknessInput, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        const auto &session = impl_->controller->session();
        impl_->applyThickness->setEnabled(session.document() && !impl_->controller->hasWork() &&
                                          value > 0.0);
    });
    connect(impl_->applyThickness, &QPushButton::clicked, this, [this] {
        const auto validation =
            impl_->controller->setManualThickness(impl_->thicknessInput->value());
        if (validation == ThicknessValidation::ConflictsWithGeometry ||
            validation == ThicknessValidation::Invalid) {
            QMessageBox::warning(
                this, QStringLiteral("Толщина не применена"),
                QStringLiteral("Значение недопустимо или противоречит определённой толщине модели. "
                               "CAD-геометрия не изменяется."));
        }
    });
    impl_->automaticThickness =
        new QPushButton(QStringLiteral("Использовать автоопределение"), panel);
    impl_->automaticThickness->setObjectName(QStringLiteral("automaticThickness"));
    layout->addWidget(impl_->automaticThickness);
    connect(impl_->automaticThickness, &QPushButton::clicked, impl_->controller,
            &ImportController::useAutomaticThickness);
    impl_->pairs = new QComboBox(panel);
    impl_->pairs->setObjectName(QStringLiteral("thicknessEvidence"));
    impl_->pairs->setAccessibleName(QStringLiteral("Пары поверхностей для измерения толщины"));
    layout->addWidget(impl_->pairs);
    impl_->showPair = new QPushButton(QStringLiteral("Показать поверхности"), panel);
    impl_->showPair->setObjectName(QStringLiteral("showThicknessFaces"));
    layout->addWidget(impl_->showPair);
    connect(impl_->showPair, &QPushButton::clicked, this,
            [this] { impl_->showThicknessEvidence(true); });
    connect(impl_->pairs, &QComboBox::currentIndexChanged, this,
            [this](int) { impl_->showThicknessEvidence(true); });
    connect(impl_->bendList->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        impl_->viewingRoute = false;
        impl_->viewingDraft = false;
        if (impl_->scenePreview() || impl_->controller->isPreparingPreview()) {
            impl_->controller->discardBendPreview();
        }
        impl_->showSelectedBend(true);
        impl_->refreshPreview();
    });
    impl_->selection = label(QStringLiteral("faceProperties"));
    impl_->sheetSides = new QCheckBox(QStringLiteral("Стороны A/B"), panel);
    impl_->sheetSides->setObjectName(QStringLiteral("sheetSidesToggle"));
    impl_->sheetSides->setMinimumHeight(32);
    impl_->sheetSides->setToolTip(
        QStringLiteral("Две стороны листа сохраняют цвет при гибке и развороте детали. Метка 1 "
                       "показывает исходный край."));
    impl_->sheetSides->setEnabled(false);
    layout->addWidget(impl_->sheetSides);
    if (auto *action = impl_->viewer->sheetOrientationAction()) {
        const auto sync = [action, toggle = impl_->sheetSides] {
            const QSignalBlocker blocked(toggle);
            toggle->setChecked(action->isChecked());
            toggle->setEnabled(action->isEnabled());
        };
        connect(action, &QAction::changed, this, sync);
        connect(impl_->sheetSides, &QCheckBox::toggled, action, &QAction::setChecked);
        sync();
    }
    auto *measurementsToggle = new QToolButton(panel);
    impl_->measurementsToggle = measurementsToggle;
    measurementsToggle->setObjectName(QStringLiteral("measurementsToggle"));
    measurementsToggle->setText(QStringLiteral("Измерения и ручные параметры ▸"));
    measurementsToggle->setCheckable(true);
    measurementsToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    measurementsToggle->setMinimumHeight(32);
    auto *measurements = new QWidget(panel);
    impl_->measurements = measurements;
    auto *measurementsLayout = new QVBoxLayout(measurements);
    measurementsLayout->setContentsMargins(0, 0, 0, 0);
    measurementsLayout->addWidget(impl_->project);
    for (auto *w :
         {static_cast<QWidget *>(impl_->properties), static_cast<QWidget *>(impl_->thicknessInput),
          static_cast<QWidget *>(impl_->applyThickness),
          static_cast<QWidget *>(impl_->automaticThickness), static_cast<QWidget *>(impl_->pairs),
          static_cast<QWidget *>(impl_->showPair), static_cast<QWidget *>(impl_->selection)}) {
        layout->removeWidget(w);
        measurementsLayout->addWidget(w);
    }
    layout->addWidget(measurementsToggle);
    layout->addWidget(measurements);
    measurements->hide();
    connect(measurementsToggle, &QToolButton::toggled, measurements, &QWidget::setVisible);
    impl_->error = label(QStringLiteral("importError"));
    impl_->error->setStyleSheet(QStringLiteral("color:#9D2828;"));
    impl_->diagnostics = new QPlainTextEdit(panel);
    impl_->diagnostics->setObjectName(QStringLiteral("importDiagnostics"));
    impl_->diagnostics->setReadOnly(true);
    impl_->diagnostics->setMaximumHeight(150);
    impl_->diagnostics->setAccessibleName(QStringLiteral("Подробности диагностики импорта"));
    layout->addWidget(impl_->diagnostics);
    auto *editBend = new QPushButton(QStringLiteral("Параметры сгиба…"), panel);
    impl_->editBend = editBend;
    editBend->setObjectName(QStringLiteral("editSelectedBend"));
    editBend->setFlat(true);
    layout->addWidget(editBend);
    connect(editBend, &QPushButton::clicked, this,
            [this] { impl_->propertyTabs->setCurrentIndex(1); });
    impl_->changePartTools = new QPushButton(QStringLiteral("Изменить инструмент…"), panel);
    impl_->changePartTools->setFlat(true);
    layout->addWidget(impl_->changePartTools);
    connect(impl_->changePartTools, &QPushButton::clicked, this,
            [this] { impl_->propertyTabs->setCurrentIndex(3); });
    layout->addStretch();
    impl_->partPrimary = new QPushButton(panel);
    impl_->partPrimary->setObjectName(QStringLiteral("partPrimaryAction"));
    impl_->partPrimary->setMinimumHeight(44);
    layout->addWidget(impl_->partPrimary);
    connect(impl_->partPrimary, &QPushButton::clicked, this, [this, open] {
        if (!impl_->controller->session().document()) {
            open->trigger();
        } else {
            impl_->propertyTabs->setCurrentIndex(3);
        }
    });
    auto *partPage = taskPage(panel, impl_->partPrimary, this);
    auto *bendPanel = new QWidget(this);
    auto *bendLayout = new QVBoxLayout(bendPanel);
    impl_->bendBindingWarning = new QLabel(bendPanel);
    impl_->bendBindingWarning->setObjectName(QStringLiteral("bendBindingWarning"));
    impl_->bendBindingWarning->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->bendBindingWarning->setWordWrap(true);
    impl_->bendBindingWarning->setTextFormat(Qt::PlainText);
    impl_->bendBindingWarning->setStyleSheet(QStringLiteral("color:#9D2828;"));
    bendLayout->addWidget(impl_->bendBindingWarning);
    impl_->bendProperties = new QLabel(bendPanel);
    impl_->bendProperties->setObjectName(QStringLiteral("bendProperties"));
    impl_->bendProperties->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->bendProperties->setWordWrap(true);
    impl_->bendProperties->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    impl_->bendProperties->setTextFormat(Qt::PlainText);
    impl_->bendProperties->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bendLayout->addWidget(impl_->bendProperties);
    bendLayout->addWidget(new QLabel(QStringLiteral("Решение по зоне сгиба"), bendPanel));
    impl_->bendReview = new QComboBox(bendPanel);
    impl_->bendReview->setObjectName(QStringLiteral("bendReview"));
    impl_->bendReview->setAccessibleName(QStringLiteral("Подтверждение зоны сгиба"));
    for (const auto review : {BendReview::Pending, BendReview::Confirmed, BendReview::Excluded,
                              BendReview::Automatic}) {
        impl_->bendReview->addItem(reviewText(review));
    }
    bendLayout->addWidget(impl_->bendReview);
    connect(impl_->bendReview, &QComboBox::activated, this, [this](int index) {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0 && index >= 0 && index <= 3) {
            impl_->controller->setBendReview(static_cast<std::size_t>(row),
                                             static_cast<BendReview>(index));
        }
    });
    impl_->bendTargetStatus = new QLabel(bendPanel);
    impl_->bendTargetStatus->setObjectName(QStringLiteral("bendTargetStatus"));
    impl_->bendTargetStatus->setTextFormat(Qt::PlainText);
    impl_->bendTargetStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->bendTargetStatus->setWordWrap(true);
    bendLayout->addWidget(impl_->bendTargetStatus);
    impl_->targetGroup = new QGroupBox(QStringLiteral("Целевые параметры сгиба"), bendPanel);
    auto *targetLayout = new QFormLayout(impl_->targetGroup);
    targetLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    targetLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    impl_->targetAngle = new QDoubleSpinBox(impl_->targetGroup);
    impl_->targetAngle->setObjectName(QStringLiteral("bendTargetAngle"));
    impl_->targetAngle->setAccessibleName(QStringLiteral("Целевой угол поворота в градусах"));
    impl_->targetAngle->setDecimals(6);
    impl_->targetAngle->setRange(1.0, 179.0);
    impl_->targetAngle->setSuffix(QStringLiteral("°"));
    targetLayout->addRow(QStringLiteral("Поворот от плоскости:"), impl_->targetAngle);
    impl_->targetRadius = new QDoubleSpinBox(impl_->targetGroup);
    impl_->targetRadius->setObjectName(QStringLiteral("bendTargetRadius"));
    impl_->targetRadius->setAccessibleName(
        QStringLiteral("Целевой внутренний радиус в миллиметрах"));
    impl_->targetRadius->setDecimals(6);
    impl_->targetRadius->setRange(0.0, 1000000.0);
    impl_->targetRadius->setSuffix(QStringLiteral(" мм"));
    targetLayout->addRow(QStringLiteral("Внутренний радиус:"), impl_->targetRadius);
    auto *targetHint = new QLabel(
        QStringLiteral("0° — плоский лист, 90° — прямой сгиб. Поддерживаемое задание: 1–179°. "
                       "Это целевая форма, не угол хода пресса.\n\n"
                       "Для распознанной зоны автоматически используются параметры CAD. "
                       "Исключённая зона не участвует в проверке. Сохранённые неподтверждённые "
                       "решения требуют выбора режима зоны."),
        bendPanel);
    targetHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    targetHint->setWordWrap(true);
    impl_->applyBendTarget =
        new QPushButton(QStringLiteral("Применить задание"), impl_->targetGroup);
    impl_->applyBendTarget->setObjectName(QStringLiteral("applyBendTarget"));
    impl_->resetBendTarget =
        new QPushButton(QStringLiteral("Сбросить задание"), impl_->targetGroup);
    impl_->resetBendTarget->setObjectName(QStringLiteral("resetBendTarget"));
    targetLayout->addRow(impl_->applyBendTarget);
    targetLayout->addRow(impl_->resetBendTarget);
    bendLayout->addWidget(impl_->targetGroup);
    bendLayout->addWidget(targetHint);
    auto *backToPart = new QPushButton(QStringLiteral("← К детали"), bendPanel);
    bendLayout->insertWidget(0, backToPart);
    connect(backToPart, &QPushButton::clicked, this,
            [this] { impl_->propertyTabs->setCurrentIndex(0); });
    bendLayout->addStretch();
    connect(impl_->targetAngle, &QDoubleSpinBox::valueChanged, this,
            [this] { impl_->updateTargetButtons(); });
    connect(impl_->targetRadius, &QDoubleSpinBox::valueChanged, this,
            [this] { impl_->updateTargetButtons(); });
    connect(impl_->applyBendTarget, &QPushButton::clicked, this, [this] {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0) {
            impl_->controller->setBendTarget(
                static_cast<std::size_t>(row),
                core::BendTarget{impl_->targetAngle->value() * std::numbers::pi / 180.0,
                                 impl_->targetRadius->value()});
        }
    });
    connect(impl_->resetBendTarget, &QPushButton::clicked, this, [this] {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0) {
            impl_->controller->setBendTarget(static_cast<std::size_t>(row), {});
        }
    });
    auto *bendScroll = new QScrollArea(this);
    bendScroll->setWidgetResizable(true);
    bendScroll->setMinimumWidth(300);
    bendScroll->setWidget(bendPanel);
    impl_->propertyTabs = new QTabWidget(this);
    impl_->propertyTabs->setObjectName(QStringLiteral("propertiesTabs"));
    impl_->propertyTabs->tabBar()->hide();
    impl_->propertyTabs->addTab(partPage, QStringLiteral("Деталь"));
    impl_->propertyTabs->addTab(bendScroll, QStringLiteral("Сгиб"));
    auto *operationPanel = new QWidget(this);
    auto *operationLayout = new QVBoxLayout(operationPanel);
    operationLayout->setContentsMargins(18, 18, 18, 8);
    auto *operationTitle = new QLabel(QStringLiteral("Гибка"), operationPanel);
    operationTitle->setStyleSheet(QStringLiteral("font-size:18px;font-weight:500;"));
    operationTitle->setWordWrap(true);
    operationLayout->addWidget(operationTitle);
    impl_->operationSummary = new QLabel(operationPanel);
    impl_->operationSummary->setObjectName("selectedOperationSummary");
    impl_->operationSummary->setWordWrap(true);
    impl_->operationSummary->setTextFormat(Qt::PlainText);
    operationLayout->addWidget(impl_->operationSummary);
    impl_->checkFlatButton = new QPushButton(operationPanel);
    impl_->checkFlatButton->setObjectName(QStringLiteral("checkFlatPattern"));
    connect(impl_->checkFlatButton, &QPushButton::clicked, impl_->flatAction, &QAction::trigger);
    operationLayout->addWidget(impl_->checkFlatButton);
    impl_->checkFlatSource = new QPushButton(QStringLiteral("Показать деталь"), operationPanel);
    connect(impl_->checkFlatSource, &QPushButton::clicked, this, [this] {
        impl_->showSource();
        impl_->refreshFlat();
    });
    operationLayout->addWidget(impl_->checkFlatSource);
    impl_->checkFlatStatus = new QLabel(operationPanel);
    impl_->checkFlatStatus->setWordWrap(true);
    operationLayout->addWidget(impl_->checkFlatStatus);
    impl_->checkBend = new QPushButton(QStringLiteral("Проверить гибку"), operationPanel);
    impl_->checkBend->setObjectName(QStringLiteral("checkBend"));
    impl_->checkBend->setMinimumHeight(40);

    impl_->fixedSide = new QComboBox(operationPanel);
    impl_->fixedSide->setObjectName(QStringLiteral("fixedBendSide"));
    impl_->fixedSide->setAccessibleName(QStringLiteral("Неподвижная полка сгиба"));
    impl_->fixedSide->addItems({QStringLiteral("Полка 1"), QStringLiteral("Полка 2")});

    impl_->preparePreview = new QPushButton(QStringLiteral("Подготовить просмотр"), operationPanel);
    impl_->preparePreview->setObjectName(QStringLiteral("prepareBendPreview"));

    impl_->previewStatus = new QLabel(operationPanel);
    impl_->previewStatus->setObjectName(QStringLiteral("bendPreviewStatus"));
    impl_->previewStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->previewStatus->setWordWrap(true);
    impl_->previewStatus->setTextFormat(Qt::PlainText);
    impl_->previewStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    impl_->checkSummary = new QLabel(operationPanel);
    impl_->checkSummary->setObjectName(QStringLiteral("bendCheckSummary"));
    impl_->checkSummary->setTextFormat(Qt::PlainText);
    impl_->checkSummary->setWordWrap(true);
    impl_->checkSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->checkSummary->setStyleSheet(
        QStringLiteral("background:#F0F3F6;color:#1C2938;border-radius:4px;padding:12px;"));
    operationLayout->addWidget(impl_->checkSummary);
    impl_->resolveCheck = new QPushButton(operationPanel);
    impl_->resolveCheck->setObjectName(QStringLiteral("resolveCheckProblem"));
    operationLayout->addWidget(impl_->resolveCheck);
    connect(impl_->resolveCheck, &QPushButton::clicked, this, [this] {
        if (impl_->checkAction == Impl::CheckAction::Tools) {
            if (impl_->stageToolIssueIndex && !impl_->stageSetupIssue.isEmpty())
                impl_->editRouteTool(*impl_->stageToolIssueIndex);
            else
                impl_->propertyTabs->setCurrentIndex(3);
        } else if (impl_->checkAction == Impl::CheckAction::Bend) {
            impl_->propertyTabs->setCurrentIndex(1);
        } else {
            impl_->showSource();
            impl_->propertyTabs->setCurrentIndex(0);
            if (impl_->checkAction == Impl::CheckAction::Thickness) {
                impl_->measurementsToggle->setChecked(true);
                impl_->thicknessInput->setFocus();
            }
        }
        impl_->refreshPreview();
    });
    impl_->checkScope = new QLabel(operationPanel);
    impl_->checkScope->setObjectName(QStringLiteral("bendCheckScope"));
    impl_->checkScope->setWordWrap(true);
    impl_->checkScope->setTextFormat(Qt::PlainText);
    impl_->checkScope->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->checkScope->setStyleSheet(QStringLiteral("color:#526171;font-size:12px;"));
    operationLayout->addWidget(impl_->checkScope);
    auto *reportToggle = new QToolButton(operationPanel);
    reportToggle->setObjectName(QStringLiteral("checkReportToggle"));
    reportToggle->setText(QStringLiteral("Подробности проверки ▸"));
    reportToggle->setCheckable(true);
    reportToggle->setMinimumHeight(32);
    operationLayout->addWidget(reportToggle);
    auto *report = new QWidget(operationPanel);
    report->setObjectName("checkReportDetails");
    auto *reportLayout = new QVBoxLayout(report);
    reportLayout->setContentsMargins(0, 0, 0, 0);
    operationLayout->removeWidget(impl_->checkScope);
    reportLayout->addWidget(impl_->checkScope);
    reportLayout->addWidget(impl_->previewStatus);
    impl_->shortToolDiagnostic = new QToolButton(report);
    impl_->shortToolDiagnostic->setObjectName("shortToolDiagnostic");
    impl_->shortToolDiagnostic->setText(QStringLiteral("Диагностический просмотр"));
    impl_->shortToolDiagnostic->setAccessibleName(
        QStringLiteral("Диагностический просмотр с коротким инструментом"));
    impl_->shortToolDiagnostic->setCheckable(true);
    impl_->shortToolDiagnostic->setToolTip(QStringLiteral(
        "Разрешить движение только для осмотра текущего гиба. Недостаточная длина остаётся "
        "ошибкой; столкновения проверяются и останавливают воспроизведение. "
        "При смене шага или инструмента разрешение сбрасывается."));
    impl_->shortToolDiagnostic->hide();
    reportLayout->addWidget(impl_->shortToolDiagnostic);
    connect(impl_->shortToolDiagnostic, &QToolButton::toggled, this, [this](bool enabled) {
        const auto draft = impl_->viewingDraft ? impl_->controller->routeDraft() : nullptr;
        impl_->shortToolDiagnosticDraft.reset();
        if (enabled && draft && draft->toolLengths && draft->request.step > 0 &&
            !impl_->viewingSource && impl_->propertyTabs->currentIndex() == 2 &&
            !impl_->controller->hasWork())
            impl_->shortToolDiagnosticDraft = draft;
        impl_->refreshPreview();
        impl_->showPreviewFrame();
    });
    reportLayout->addWidget(impl_->routePanel->reportWidget());
    operationLayout->addWidget(report);
    report->hide();
    connect(reportToggle, &QToolButton::toggled, report, &QWidget::setVisible);
    operationLayout->addWidget(impl_->routePanel);
    auto *kinematicHint = new QLabel(
        QStringLiteral("Модель сохраняет толщину и длину средней поверхности. Это геометрическая "
                       "кинематика.\n\n"
                       "Здесь инструмент и контакт не участвуют. Начальная установка доступна на "
                       "вкладке «Инструмент». Усилие и пружинение не рассчитываются. "
                       "Это не технологическая развёртка для производства.\n\nВыбор полки и кадра "
                       "относится к просмотру и не сохраняется в проекте."),
        operationPanel);
    kinematicHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    kinematicHint->setWordWrap(true);
    auto *advancedToggle = new QToolButton(operationPanel);
    impl_->kinematicsToggle = advancedToggle;
    advancedToggle->setText(QStringLiteral("Движение без инструмента ▸"));
    advancedToggle->setCheckable(true);
    advancedToggle->setMinimumHeight(32);
    auto *advanced = new QWidget(operationPanel);
    impl_->kinematicsPanel = advanced;
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->addWidget(impl_->fixedSide);
    advancedLayout->addWidget(impl_->preparePreview);
    advancedLayout->addWidget(kinematicHint);
    reportLayout->addWidget(advancedToggle);
    reportLayout->addWidget(advanced);
    advanced->hide();
    connect(advancedToggle, &QToolButton::toggled, advanced, &QWidget::setVisible);
    operationLayout->addStretch();
    operationLayout->addWidget(impl_->checkBend);
    auto *operationPage = taskPage(operationPanel, impl_->checkBend, this);
    impl_->propertyTabs->addTab(operationPage, QStringLiteral("Операция"));
    auto *toolPanel = new QWidget(this);
    auto *toolLayout = new QVBoxLayout(toolPanel);
    toolLayout->setContentsMargins(18, 18, 18, 8);
    auto *toolHeading = new QLabel(QStringLiteral("Гибочный инструмент"), toolPanel);
    toolLayout->addWidget(toolHeading);
    impl_->toolAssignmentScope = new QComboBox(toolPanel);
    impl_->toolAssignmentScope->setObjectName("toolAssignmentScope");
    impl_->toolAssignmentScope->setAccessibleName(QStringLiteral("Назначение инструмента"));
    impl_->toolAssignmentScope->setToolTip(
        QStringLiteral("Шаг 0 — вся деталь. Другие шаги — выбранный гиб. "
                       "Здесь можно изменить область назначения без расчёта."));
    impl_->toolAssignmentScope->addItem(QStringLiteral("Вся деталь"), QString{});
    toolLayout->addWidget(impl_->toolAssignmentScope);
    impl_->applyTools = new QPushButton(QStringLiteral("Проверить гибку"), toolPanel);
    impl_->applyTools->setObjectName(QStringLiteral("applyToolSetup"));
    impl_->applyTools->setMinimumHeight(40);

    impl_->toolSelection = new ToolSelectionWidget(std::move(catalog), toolPanel);
    impl_->toolWorkspace->bindSelection(impl_->toolSelection);
    impl_->toolSelection->onAppearanceChanged = [this] {
        impl_->viewer->setToolSegmentColors(toolSegmentColors());
    };
    toolLayout->addWidget(impl_->toolSelection);
    impl_->assignBendTool = new QPushButton(toolPanel);
    impl_->assignBendTool->setObjectName("assignBendTool");
    impl_->assignBendTool->setToolTip(
        QStringLiteral("Сохранить только выбранному гибу без расчёта"));
    toolLayout->addWidget(impl_->assignBendTool);
    impl_->toolAssignmentStatus = new QLabel(toolPanel);
    impl_->toolAssignmentStatus->setObjectName("toolAssignmentStatus");
    impl_->toolAssignmentStatus->setWordWrap(true);
    toolLayout->addWidget(impl_->toolAssignmentStatus);
    impl_->toolMode = new QComboBox(toolPanel);
    impl_->toolMode->setObjectName(QStringLiteral("toolPreviewMode"));
    impl_->toolMode->setAccessibleName(QStringLiteral("Режим просмотра инструмента"));
    impl_->toolMode->setToolTip(QStringLiteral("Режим применяется кнопкой подготовки"));
    impl_->toolMode->addItems(
        {QStringLiteral("Гибка с инструментом"), QStringLiteral("Начальная установка")});
    auto *extraTools = new QWidget(toolPanel);
    auto *extraToolsLayout = new QVBoxLayout(extraTools);
    extraToolsLayout->setContentsMargins(0, 0, 0, 0);
    auto *extraToolsToggle = new QToolButton(toolPanel);
    extraToolsToggle->setText(QStringLiteral("Другие виды просмотра ▸"));
    extraToolsToggle->setCheckable(true);
    extraToolsToggle->setMinimumHeight(30);
    toolLayout->addWidget(extraToolsToggle);
    toolLayout->addWidget(extraTools);
    extraToolsLayout->addWidget(impl_->toolMode);
    extraTools->hide();
    connect(extraToolsToggle, &QToolButton::toggled, extraTools, &QWidget::setVisible);
    connect(impl_->toolMode, &QComboBox::currentIndexChanged, this, [this](int) {
        impl_->setupAttempt.reset();
        impl_->controller->discardBendPreview();
    });
    impl_->clearTools = new QPushButton(QStringLiteral("Удалить настройку инструмента"), toolPanel);
    impl_->clearTools->setObjectName(QStringLiteral("clearToolSetup"));
    extraToolsLayout->addWidget(impl_->clearTools);
    impl_->toolStatus = new QLabel(toolPanel);
    impl_->toolStatus->setObjectName(QStringLiteral("toolSetupStatus"));
    impl_->toolStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->toolStatus->setWordWrap(true);
    impl_->toolStatus->setTextFormat(Qt::PlainText);
    impl_->toolStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolLayout->addWidget(impl_->toolStatus);
    toolLayout->addStretch();
    toolLayout->addWidget(impl_->applyTools);
    auto *toolPage = taskPage(toolPanel, impl_->applyTools, this);
    impl_->propertyTabs->addTab(toolPage, QStringLiteral("Инструмент"));
    connect(impl_->applyTools, &QPushButton::clicked, this,
            [this] { impl_->enterBendingStage(true); });
    connect(impl_->assignBendTool, &QPushButton::clicked, this,
            [this] { impl_->assignSelectedTool(); });
    connect(impl_->toolAssignmentScope, &QComboBox::currentIndexChanged, this, [this](int at) {
        if (at < 0)
            return;
        const auto id = impl_->toolAssignmentScope->itemData(at).toString().toStdString();
        if (id == impl_->toolContextId)
            return;
        impl_->cancelCheckedToolSelection(true);
        std::optional<std::size_t> index;
        for (const auto i : impl_->controller->session().orderedBends())
            if (impl_->controller->session().bend(i)->id == id)
                index = i;
        impl_->setToolContext(index);
        impl_->refreshPreview();
    });
    connect(impl_->checkBend, &QPushButton::clicked, this, [this] {
        impl_->toolMode->setCurrentIndex(0);
        impl_->checkTools();
    });
    impl_->toolSelection->onChanged = [this](ToolSelectionWidget::Change selected) {
        impl_->cancelCheckedToolSelection(true);
        impl_->beginPreviewReplacement(true);
        impl_->setupAttempt.reset();
        const bool firstChange = !impl_->toolDraftChanged;
        impl_->toolDraftChanged = true;
        impl_->toolDraftPunchSelected |= selected == ToolSelectionWidget::Change::Punch ||
                                         selected == ToolSelectionWidget::Change::Pair;
        impl_->toolDraftDieSelected |= selected == ToolSelectionWidget::Change::Die ||
                                       selected == ToolSelectionWidget::Change::Pair;
        impl_->stageSetupIssue.clear();
        impl_->stageToolIssueIndex.reset();
        if (firstChange)
            impl_->controller->discardToolChecks();
        else
            impl_->refreshPreview();
    };
    impl_->checkedToolDialog = new ToolSearchDialog(this);
    impl_->checkedToolDialog->onCancel = [this] { impl_->cancelCheckedToolSelection(); };
    impl_->checkedToolDialog->onContinue = [this] { impl_->prepareCheckedToolSelection(); };
    impl_->checkedToolDialog->onQuick = [this] {
        impl_->checkedToolDialog->hide();
        findChild<QAction *>("recommendToolsQuick")->trigger();
        impl_->propertyTabs->setCurrentIndex(3);
    };
    impl_->checkedToolDialog->onManual = [this] {
        impl_->cancelCheckedToolSelection(true);
        impl_->checkedToolDialog->hide();
        impl_->propertyTabs->setCurrentIndex(3);
        impl_->toolWorkspace->setFocus();
    };
    impl_->toolSelection->onCheckedRecommend = [this] { impl_->prepareCheckedToolSelection(); };
    impl_->toolSelection->onCancelChecked = [this] { impl_->cancelCheckedToolSelection(true); };
    connect(impl_->clearTools, &QPushButton::clicked, this, [this] {
        std::vector<application::BendToolUpdate> updates;
        for (const auto i : impl_->controller->session().orderedBends()) {
            const auto *entry = impl_->controller->session().bend(i);
            if (included(entry->review) &&
                (impl_->toolContextId.empty() || impl_->toolContextId == entry->id))
                updates.push_back({i, {}, {}});
        }
        impl_->beginPreviewReplacement();
        impl_->toolDraftChanged = false;
        impl_->controller->setBendToolAssignments(updates);
        impl_->refreshPreview();
    });
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Текущая задача"), impl_->propertyTabs,
            Qt::RightDockWidgetArea);
    impl_->propertyTabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane{border:0;background:white;}QScrollArea{border:0;background:white;}"));
    resizeDocks({findChild<QDockWidget *>(QStringLiteral("propertiesDock"))}, {350},
                Qt::Horizontal);
    impl_->operationOverlay = new QWidget(this);
    impl_->operationOverlay->setObjectName("operationOverlay");
    auto *overlayLayout = new QVBoxLayout(impl_->operationOverlay);
    overlayLayout->setContentsMargins(6, 6, 6, 6);
    overlayLayout->setSpacing(2);
    overlayLayout->addWidget(impl_->routePanel->stepStrip(), 0, Qt::AlignHCenter);
    auto *sequence = new QWidget(impl_->operationOverlay);
    sequence->setObjectName("playbackControls");
    impl_->playbackControls = sequence;
    overlayLayout->addWidget(sequence);
    auto *sequenceLayout = new QVBoxLayout(sequence);
    sequenceLayout->setContentsMargins(0, 0, 0, 0);
    sequenceLayout->setSpacing(0);
    impl_->frameLabel = new QLabel(sequence);
    impl_->frameLabel->setObjectName(QStringLiteral("bendFrameLabel"));
    impl_->frameLabel->hide(); // Full frame detail is available as a transport tooltip.
    impl_->setupPlaybackHint =
        new QLabel(QStringLiteral("Здесь показано одно начальное "
                                  "положение. Для движения выберите «Гибка с инструментом» на "
                                  "вкладке «Инструмент» и подготовьте просмотр."),
                   sequence);
    impl_->setupPlaybackHint->setObjectName(QStringLiteral("setupPlaybackHint"));
    impl_->setupPlaybackHint->setWordWrap(true);

    impl_->bendFrame = new QSlider(Qt::Horizontal, sequence);
    impl_->bendFrame->setObjectName(QStringLiteral("bendFrame"));
    impl_->bendFrame->setAccessibleName(QStringLiteral("Промежуточное состояние сгиба"));
    impl_->bendFrame->setRange(0, 20);
    impl_->bendFrame->setTickPosition(QSlider::NoTicks);
    impl_->bendFrame->setTickInterval(5);
    impl_->previewControls = new QStackedWidget(sequence);
    impl_->previewControls->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    impl_->previewControls->addWidget(impl_->bendFrame);
    impl_->previewControls->addWidget(impl_->setupPlaybackHint);

    auto *playLayout = new QHBoxLayout;
    impl_->playBend = new QPushButton(QStringLiteral("Воспроизвести"), sequence);
    impl_->playBend->setObjectName(QStringLiteral("playBend"));
    impl_->returnToCad = new QPushButton(QStringLiteral("Деталь"), sequence);
    impl_->returnToCad->setObjectName(QStringLiteral("returnToCad"));
    playLayout->addWidget(impl_->playBend);
    impl_->reversePunchButton = new QToolButton(sequence);
    impl_->reversePunchButton->setObjectName("reversePunch");
    impl_->punchOrientationIcons = {punchOrientationIcon(false), punchOrientationIcon(true)};
    impl_->reversePunchButton->setIcon(impl_->punchOrientationIcons[0]);
    impl_->reversePunchButton->setCheckable(true);
    impl_->reversePunchButton->setIconSize(QSize(42, 28));
    impl_->reversePunchButton->setFixedSize(50, 34);
    impl_->reversePunchButton->setAccessibleName(QStringLiteral("Переключить ориентацию пуансона"));
    impl_->reversePunchButton->setStyleSheet(QStringLiteral(
        "QToolButton{background:#F7F9FA;border:1px solid #C5CFD8;border-radius:4px;padding:1px;}"
        "QToolButton:checked{background:#D8EEEE;border:2px solid #087F83;}"
        "QToolButton:hover:!checked{background:#E8EEF2;}QToolButton:disabled{border-color:#DCE3E9;"
        "}"));
    playLayout->addWidget(impl_->reversePunchButton);
    impl_->reverseWorkpieceButton = new QToolButton(sequence);
    impl_->reverseWorkpieceButton->setObjectName("reverseWorkpiece");
    impl_->workpieceOrientationIcons = {workpieceOrientationIcon(false),
                                        workpieceOrientationIcon(true)};
    impl_->reverseWorkpieceButton->setIcon(impl_->workpieceOrientationIcons[0]);
    impl_->reverseWorkpieceButton->setCheckable(true);
    impl_->reverseWorkpieceButton->setIconSize(QSize(42, 28));
    impl_->reverseWorkpieceButton->setFixedSize(50, 34);
    impl_->reverseWorkpieceButton->setAccessibleName(QStringLiteral("Развернуть деталь на 180°"));
    impl_->reverseWorkpieceButton->setStyleSheet(impl_->reversePunchButton->styleSheet());
    playLayout->addWidget(impl_->reverseWorkpieceButton);
    connect(impl_->reverseWorkpieceButton, &QToolButton::clicked, this,
            [this](bool reversed) { impl_->setWorkpieceOrientation(reversed); });
    connect(impl_->reversePunchButton, &QToolButton::clicked, this,
            [this](bool reversed) { impl_->setPunchOrientation(reversed); });
    playLayout->addWidget(impl_->previewControls, 1);
    impl_->showKinematics =
        new QPushButton(QStringLiteral("Подготовить движение без инструмента"), sequence);
    impl_->showKinematics->setObjectName(QStringLiteral("showKinematics"));
    connect(impl_->showKinematics, &QPushButton::clicked, impl_->preparePreview,
            &QPushButton::click);
    playLayout->addWidget(impl_->showKinematics);
    impl_->playbackSpeedControls = new QWidget(sequence);
    auto *speedLayout = new QHBoxLayout(impl_->playbackSpeedControls);
    speedLayout->setContentsMargins(8, 0, 8, 0);
    auto *speedTitle = new QLabel(QStringLiteral("Скорость"), impl_->playbackSpeedControls);
    impl_->playbackSpeed = new QSlider(Qt::Horizontal, impl_->playbackSpeedControls);
    impl_->playbackSpeed->setObjectName(QStringLiteral("bendPlaybackSpeed"));
    impl_->playbackSpeed->setAccessibleName(QStringLiteral("Скорость воспроизведения"));
    impl_->playbackSpeed->setRange(0, 8); // 1x to 3x, in quarter steps.
    impl_->playbackSpeed->setPageStep(2);
    impl_->playbackSpeed->setTickPosition(QSlider::NoTicks);
    impl_->playbackSpeed->setTickInterval(1);
    impl_->playbackSpeed->setMinimumWidth(80);
    impl_->playbackSpeed->setMaximumWidth(95);
    speedTitle->setBuddy(impl_->playbackSpeed);
    impl_->playbackSpeedValue = new QLabel(QStringLiteral("1×"), impl_->playbackSpeedControls);
    impl_->playbackSpeedValue->setObjectName(QStringLiteral("bendPlaybackSpeedValue"));
    impl_->playbackSpeedValue->setMinimumWidth(40);
    speedLayout->addWidget(speedTitle);
    speedLayout->addWidget(impl_->playbackSpeed);
    speedLayout->addWidget(impl_->playbackSpeedValue);
    playLayout->addWidget(impl_->playbackSpeedControls);
    playLayout->addWidget(impl_->returnToCad);
    playLayout->addStretch();
    sequenceLayout->addLayout(playLayout);
    impl_->playback = new QTimer(this);
    impl_->playback->setObjectName(QStringLiteral("bendPlaybackTimer"));
    impl_->playback->setInterval(150);
    const auto updatePlaybackSpeed = [this](int step) {
        const double multiplier = 1.0 + static_cast<double>(step) / 4.0;
        const auto value = number(multiplier) + QStringLiteral("×");
        impl_->playback->setInterval(qRound(150.0 / multiplier));
        impl_->playbackSpeedValue->setText(value);
        const auto description = QStringLiteral("Скорость показа %1. Диапазон 1×–3×, шаг 0,25×. "
                                                "Скорость пресса не рассчитывается.")
                                     .arg(value);
        impl_->playbackSpeed->setToolTip(description);
        impl_->playbackSpeed->setAccessibleDescription(description);
    };
    connect(impl_->playbackSpeed, &QSlider::valueChanged, this, updatePlaybackSpeed);
    updatePlaybackSpeed(impl_->playbackSpeed->value());
    connect(impl_->bendFrame, &QSlider::valueChanged, this, [this] { impl_->showPreviewFrame(); });
    connect(impl_->playback, &QTimer::timeout, this, [this] {
        if (impl_->shortToolPlaybackBlocked() ||
            Impl::collisionAt(impl_->scenePreview(),
                              static_cast<std::size_t>(impl_->bendFrame->value()))) {
            impl_->stopPlayback();
            return;
        }
        if (impl_->bendFrame->value() >= impl_->bendFrame->maximum()) {
            if (impl_->viewingRoute)
                if (const auto route = impl_->controller->bendRoute();
                    route &&
                    impl_->routePanel->selected() < static_cast<int>(route->steps.size())) {
                    impl_->routePanel->select(impl_->routePanel->selected() + 1);
                    impl_->bendFrame->setValue(0);
                    impl_->playBend->setText(QStringLiteral("Пауза"));
                    impl_->playback->start();
                    return;
                }
            if (impl_->viewingDraft &&
                impl_->routePanel->selected() <
                    static_cast<int>(impl_->controller->session().orderedBends().size())) {
                impl_->routePanel->select(impl_->routePanel->selected() + 1);
                impl_->draftAutoPlay = true;
                return;
            }
            impl_->stopPlayback();
        } else {
            impl_->bendFrame->setValue(impl_->bendFrame->value() + 1);
        }
    });
    connect(impl_->playBend, &QPushButton::clicked, this, [this] {
        if (impl_->playback->isActive()) {
            impl_->stopPlayback();
            return;
        }
        if (!impl_->playBend->isEnabled() || impl_->shortToolPlaybackBlocked())
            return;
        if (impl_->viewingRoute && impl_->routePanel->selected() == 0)
            impl_->routePanel->select(1);
        if (impl_->viewingDraft && impl_->routePanel->selected() == 0) {
            impl_->routePanel->select(1);
            impl_->draftAutoPlay = true;
            return;
        }
        if (impl_->bendFrame->value() == impl_->bendFrame->maximum()) {
            impl_->bendFrame->setValue(0);
        }
        if (!impl_->playBend->isEnabled())
            return;
        impl_->playBend->setText(QStringLiteral("Пауза"));
        impl_->playback->start();
    });
    connect(impl_->returnToCad, &QPushButton::clicked, this, [this] {
        impl_->showSource();
        impl_->propertyTabs->setCurrentIndex(0);
        impl_->refreshPreview();
    });
    connect(impl_->fixedSide, &QComboBox::currentIndexChanged, this, [this] {
        impl_->setupAttempt.reset();
        impl_->controller->discardBendPreview();
    });
    connect(impl_->preparePreview, &QPushButton::clicked, this, [this] {
        impl_->stageSetupIssue.clear();
        impl_->stageToolIssueIndex.reset();
        impl_->viewingRoute = false;
        impl_->viewingSource = false;
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0) {
            impl_->propertyTabs->setCurrentIndex(2);
            if (impl_->controller->prepareBendPreview(static_cast<std::size_t>(row),
                                                      impl_->fixedSide->currentIndex() == 1,
                                                      impl_->previewBuilder)) {
                impl_->showPlaybackPanel();
            }
        }
    });
    impl_->operationOverlay->setStyleSheet(QStringLiteral(
        "QWidget#operationOverlay{background:#F4F7F9;border:1px solid #526171;border-radius:6px;}"
        "QWidget#routeStepStrip{border:0;}QWidget#playbackControls{background:transparent;}"));
    impl_->viewer->setBottomOverlay(impl_->operationOverlay);
    impl_->operationOverlay->hide();
    impl_->bendMessages = new BendMessagePanel(this);
    impl_->viewer->setTopLeftOverlay(impl_->bendMessages);
    impl_->routePanel->onSelect = [this](int) {
        impl_->beginPreviewReplacement();
        impl_->stopPlayback();
        impl_->draftAutoPlay = false;
        if (impl_->controller->session().document() &&
            impl_->controller->session().document()->metadata().bendAnalysis.bends.size() == 1 &&
            !impl_->controller->bendRoute()) {
            const auto preview = impl_->controller->bendPreview();
            if (preview && preview->pressMotion()) {
                impl_->viewingDraft = false;
                impl_->viewingRoute = false;
                impl_->viewingSource = false;
                impl_->wantFlat = false;
                impl_->bendFrame->setValue(impl_->routePanel->selected() == 0
                                               ? 0
                                               : static_cast<int>(preview->frameCount() - 1));
                impl_->showPreviewFrame();
                impl_->refreshPreview();
            }
            return;
        }
        if (impl_->controller->bendRoute()) {
            impl_->viewingDraft = false;
            impl_->viewer->highlightPreviewBend(std::nullopt);
            impl_->viewingRoute = true;
            impl_->viewingSource = false;
            impl_->wantFlat = false;
            impl_->positionedPreview.reset();
        } else {
            impl_->prepareManualDraft();
            return;
        }
        impl_->refreshPreview();
        if (impl_->controller->bendRoute())
            impl_->showPlaybackPanel();
    };
    impl_->routePanel->onHover = [this](std::optional<std::size_t> i) {
        impl_->viewer->highlightPreviewBend(i);
    };
    impl_->routePanel->onModeChanged = [this] {
        impl_->controller->discardBendRoute();
        if (impl_->viewingDraft)
            impl_->prepareManualDraft();
        else
            impl_->refreshPreview();
    };
    impl_->routePanel->onEditTool = [this](std::size_t index) { impl_->editRouteTool(index); };
    impl_->routePanel->onExpandSearch = [this] {
        if (!impl_->checkedToolVariants.empty())
            impl_->prepareCheckedToolSelection();
        else
            impl_->prepareRoute(impl_->lastRouteAutomatic, impl_->routePanel->withTools(), true);
    };
    impl_->routePanel->onReorder = [this](std::vector<std::string> ids) {
        impl_->beginPreviewReplacement();
        impl_->stopPlayback();
        impl_->draftAutoPlay = false;
        if (!impl_->controller->setBendOrder(ids))
            return false;
        impl_->prepareManualDraft();
        return true;
    };
    impl_->routePanel->onPrepare = [this](bool automatic, bool withTools) {
        impl_->prepareRoute(automatic, withTools);
    };
    auto *processBar = new QToolBar(QStringLiteral("Этапы работы"), this);
    processBar->setObjectName(QStringLiteral("processNavigation"));
    processBar->setMovable(false);
    processBar->setFloatable(false);
    processBar->setAllowedAreas(Qt::BottomToolBarArea);
    processBar->setContextMenuPolicy(Qt::PreventContextMenu);
    addToolBar(Qt::BottomToolBarArea, processBar);
    auto *processPanel = new QWidget(processBar);
    auto *processLayout = new QHBoxLayout(processPanel);
    processLayout->setContentsMargins(16, 8, 16, 8);
    processLayout->setSpacing(8);
    processPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    processPanel->setStyleSheet(QStringLiteral(
        "QToolButton{background:#FFFFFF;color:#526171;border:1px solid "
        "#DCE3E9;border-radius:4px;padding:8px 16px;font-size:14px;}"
        "QToolButton:checked{background:#E5F3F3;color:#087F83;border:2px solid #087F83;}"
        "QToolButton:hover{background:#F0F3F6;}QToolButton:focus{border:2px solid #087F83;}"));
    auto *processGroup = new QButtonGroup(processPanel);
    processGroup->setExclusive(true);
    const std::array<QString, 3> stageNames{QStringLiteral("Деталь"), QStringLiteral("Инструмент"),
                                            QStringLiteral("Гибка")};
    const std::array<int, 3> stageTabs{0, 3, 2};
    for (int i = 0; i < 3; ++i) {
        auto *button =
            new ProcessStepButton(i, stageNames[static_cast<std::size_t>(i)], processPanel);
        impl_->processSteps[static_cast<std::size_t>(i)] = button;
        button->setObjectName(QStringLiteral("processStage%1").arg(i));
        button->setShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(i + 1)));
        button->setToolTip(stageNames[static_cast<std::size_t>(i)] +
                           QStringLiteral(" · Alt+%1").arg(i + 1));
        processGroup->addButton(button, i);
        processLayout->addWidget(button, 1);
        connect(button, &QToolButton::clicked, this, [this, stageTabs, i] {
            if (i == 0)
                impl_->showSource();
            else if (i == 2) {
                impl_->enterBendingStage();
                return;
            }
            impl_->propertyTabs->setCurrentIndex(stageTabs[static_cast<std::size_t>(i)]);
            impl_->propertyTabs->parentWidget()->show();
            impl_->refreshPreview();
        });
    }
    processGroup->button(0)->setChecked(true);
    auto *drawingButton = new ProcessStepButton(3, QStringLiteral("Чертёж / PDF"), processPanel);
    drawingButton->setObjectName("drawingStageButton");
    drawingButton->setStatus(QStringLiteral("Виды и развёртка"));
    drawingButton->setEnabled(impl_->drawingAction->isEnabled());
    processLayout->addWidget(drawingButton, 1);
    connect(drawingButton, &QToolButton::clicked, impl_->drawingAction, &QAction::trigger);
    connect(impl_->drawingAction, &QAction::changed, drawingButton,
            [drawingButton, action = impl_->drawingAction] {
                drawingButton->setEnabled(action->isEnabled());
            });
    connect(impl_->propertyTabs, &QTabWidget::currentChanged, this, [this, processGroup](int page) {
        const int stage = page == 3 ? 1 : page == 2 ? 2 : 0;
        processGroup->button(stage)->setChecked(true);
        if (page != 2)
            impl_->stopPlayback();
        if (page != 2)
            impl_->pendingBendStage.reset();
        impl_->workspaceStack->setCurrentIndex(page == 3 ? 1 : 0);
        if (stage == 0)
            impl_->showSource();
        impl_->refreshWorkflow();
        if (page == 3) {
            const auto index =
                impl_->routePanel->selected() == 0 ? std::nullopt : impl_->selectedPunchIndex();
            impl_->setToolContext(index);
            impl_->suggestInitialTools();
        }
    });
    processBar->addWidget(processPanel);

    impl_->progress = new QProgressBar(this);
    impl_->progress->setObjectName(QStringLiteral("importProgress"));
    impl_->progress->setRange(0, 0);
    impl_->progress->setMaximumWidth(140);
    impl_->progress->setAccessibleName(QStringLiteral("Загрузка STEP"));
    statusBar()->addPermanentWidget(impl_->progress);

    connect(impl_->controller, &ImportController::stateChanged, this, [this] {
        impl_->refresh();
        impl_->advanceUpdate();
    });
    connect(impl_->viewer, &viewer::ViewerWidget::renderingChanged, this,
            [this](bool) { impl_->refresh(); });
    connect(impl_->viewer, &viewer::ViewerWidget::selectionChanged, this,
            [this](quint64 revision, int index) {
                const auto &session = impl_->controller->session();
                if (revision == session.revision() && session.document() && index > 0 &&
                    static_cast<std::size_t>(index) <= session.document()->faces().size()) {
                    const auto &face =
                        session.document()->faces()[static_cast<std::size_t>(index - 1)];
                    impl_->drawingBase = face.surface == SurfaceKind::Plane
                                             ? std::optional{face.index}
                                             : std::nullopt;
                    impl_->drawingBaseRevision = session.revision();
                    impl_->selection->setText(
                        QStringLiteral("Грань №%1\nПоверхность: %2\nПлощадь: %3 мм²")
                            .arg(face.index)
                            .arg(surfaceName(face.surface), number(face.areaMm2)));
                } else {
                    impl_->drawingBase.reset();
                    impl_->selection->setText(
                        QStringLiteral("Грань не выбрана. Нажмите ЛКМ на модели."));
                }
            });

    auto *help = menuBar()->addMenu(QStringLiteral("Справка"));
    auto *navigationHelp = help->addAction(QStringLiteral("Управление сценой"));
    navigationHelp->setShortcut(QKeySequence::HelpContents);
    connect(navigationHelp, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, QStringLiteral("Управление сценой"),
            QStringLiteral("ПКМ — вращение, СКМ — перенос, колесо — масштаб.\n"
                           "ЛКМ — выбор грани; F — вписать, Ctrl+D — размеры.\n"
                           "Куб переключает виды. Якорь Y фиксирует горизонтальный обход пресса.\n"
                           "В ленте 0 — заготовка, номера — результаты гибов. "
                           "Перетаскивание меняет порядок; Ctrl+Z в ленте отменяет перестановку."));
    });
    impl_->updatesAction = help->addAction(QStringLiteral("Проверить обновления…"));
    impl_->updatesAction->setObjectName(QStringLiteral("checkUpdatesAction"));
    impl_->updatesAction->setEnabled(false);
    connect(impl_->updatesAction, &QAction::triggered, this, [this] {
        if (!impl_->updates)
            return;
        if (impl_->updateDialog) {
            impl_->updateDialog->showNormal();
            impl_->updateDialog->raise();
            impl_->updateDialog->activateWindow();
            return;
        }
        impl_->updateDialog = new UpdateDialog(
            *impl_->updates,
            [this] {
                requestUpdateRestart(
                    [this](const QString &path) { return impl_->updates->startInstaller(path); });
            },
            this);
        connect(impl_->updateDialog, &QDialog::finished, this, [this] {
            impl_->updateDialog.clear();
            impl_->updatePending = false;
        });
        impl_->updateDialog->show();
    });
    auto *about = help->addAction(QStringLiteral("О программе"));
    connect(about, &QAction::triggered, this, [this, cadVersion = text(runtime.cadVersion)] {
        QMessageBox::about(
            this, QStringLiteral("3D Bending Simulator"),
            impl_->title +
                QStringLiteral(
                    "\n\nГеометрическая проверка одного сгиба на упрощённом инструменте."
                    "\nПодвод и гибка проверяются по кадрам, отвод поддержанного пуансона — "
                    "непрерывно. Усилие и пружинение не рассчитываются.\nQt %1 · OCCT %2")
                    .arg(QString::fromLatin1(qVersion()), cadVersion));
    });
    impl_->refresh();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    delete impl_->partInputDialog.data();
    delete impl_->drawingDialog.data();
    delete impl_->updateDialog.data();
    impl_->controller->shutdown();
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    const auto type = event->type();
    if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop)
        return QMainWindow::eventFilter(watched, event);
    const auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != this)
        return QMainWindow::eventFilter(watched, event);
    auto *drop = static_cast<QDropEvent *>(event);
    if (!drop->mimeData() || !drop->mimeData()->hasUrls())
        return QMainWindow::eventFilter(watched, event);
    const auto path = fileDropPath(drop->mimeData(), type != QEvent::DragMove);
    if (path.isEmpty() || !(drop->possibleActions() & Qt::CopyAction) ||
        QApplication::activeModalWidget() || !isEnabled()) {
        drop->ignore();
        return true;
    }
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (type == QEvent::Drop)
        openStep(path);
    return true;
}
void MainWindow::openStep(const QString &path) {
    if (!path.isEmpty() && impl_->canDiscard()) {
        impl_->controller->open(nativePath(path));
    }
}
ImportController &MainWindow::importController() const {
    return *impl_->controller;
}
void MainWindow::setUpdateClient(update::UpdateClient *client) {
    impl_->updates = client;
    impl_->updatesAction->setEnabled(client != nullptr);
}
void MainWindow::requestUpdateRestart(std::function<QString(const QString &)> installer) {
    impl_->updateInstaller = std::move(installer);
    impl_->updatePending = static_cast<bool>(impl_->updateInstaller);
    impl_->updateSaving = false;
    impl_->updateDiscard = false;
    impl_->advanceUpdate();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!impl_->updateClosing && !impl_->canDiscard()) {
        event->ignore();
        return;
    }
    impl_->controller->closeDocument();
    QMainWindow::closeEvent(event);
}
void MainWindow::setToolCatalogLoader(
    std::function<std::variant<core::ToolCatalog, std::string>(const std::filesystem::path &)>
        loader) {
    impl_->toolSelection->loadCatalog = std::move(loader);
}
void MainWindow::setDrawingBuilder(
    std::shared_ptr<const application::BendingDrawingBuilder> builder) {
    impl_->drawingBuilder = std::move(builder);
    impl_->refresh();
}
void MainWindow::setToolProfileImport(
    std::shared_ptr<const application::ToolProfileImporter> importer,
    application::ToolLibraryStorage writer, core::ToolCatalog starter) {
    impl_->toolSelection->setToolProfileImport(*impl_->controller, std::move(importer),
                                               std::move(writer), std::move(starter));
    impl_->refresh();
}
void MainWindow::setFlatPatternBuilder(
    std::shared_ptr<const application::FlatPatternBuilder> builder) {
    impl_->flatBuilder = std::move(builder);
    impl_->refresh();
}
} // namespace bending::ui

namespace bending::ui {
void MainWindow::setAutomaticPartInputPrompt(bool enabled) {
    impl_->automaticInputPrompt = enabled;
    ++impl_->inputPromptSerial;
    if (!enabled && impl_->partInputDialog)
        impl_->partInputDialog->reject();
}
} // namespace bending::ui
