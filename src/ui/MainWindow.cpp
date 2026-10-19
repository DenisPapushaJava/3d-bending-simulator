#include "bending/ui/MainWindow.h"
#include "ProcessStepButton.h"
#include "ToolSelectionWidget.h"

#include "bending/ui/ImportController.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace bending::ui {
namespace {
using namespace application;

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
                properties->setText(QStringLiteral(
                    "Откройте STEP отдельной детали: Файл → Открыть STEP…\n\nПоддерживается одно "
                    "корректное объёмное тело.\nЕдиницы будут приведены к мм."));
                window->setWindowTitle(title);
            }
        }
        const bool newGeometry = shownBendRevision != session.revision();
        if (newGeometry) {
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
            if (document && document->metadata().bendAnalysis.bends.size() == 1)
                bendList->setCurrentIndex(bendModel->index(0, 0));
            viewingSource = false;
            propertyTabs->setCurrentIndex(0);
        }
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
                        : QStringLiteral("Толщина автоматически не определена. Поддерживаются "
                                         "плоские детали постоянной толщины.");
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
        updateBendReviews();
        showSelectedBend(false);
        camera->setEnabled(viewer->isReady() && document != nullptr);
        closeAction->setEnabled(document != nullptr || session.activeRequest().has_value());
        const bool cancellable =
            session.activeRequest().has_value() || controller->isPreparingPreview();
        cancelAction->setVisible(cancellable);
        cancelAction->setEnabled(cancellable);
        progress->setVisible(session.activeRequest().has_value() || controller->isSaving() ||
                             controller->isPreparingPreview());
        if (controller->isPreparingPreview()) {
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
            window->statusBar()->showMessage(QStringLiteral(
                "Просмотр операции · ПКМ: вращение · СКМ: перенос · Колесо: масштаб"));
        } else {
            window->statusBar()->showMessage(
                document ? QStringLiteral("Деталь загружена · ПКМ: вращение · СКМ: перенос · "
                                          "Колесо: масштаб · ЛКМ: грань")
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
    }

    void checkTools() {
        if (controller->hasWork())
            return;
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
        const auto assignment = toolSelection->assignment();
        toolDraftChanged = false;
        const bool applied =
            assignment
                ? controller->setBendToolAssignment(static_cast<std::size_t>(row), *assignment)
                : controller->setBendTools(static_cast<std::size_t>(row), *tools);
        if (!applied)
            return;
        toolSelection->rememberChoice();
        viewingSource = false;
        propertyTabs->setCurrentIndex(2);
        if (controller->prepareBendPreview(static_cast<std::size_t>(row), false, previewBuilder,
                                           toolMode->currentIndex() == 0
                                               ? BendPreviewMode::ToolMotion
                                               : BendPreviewMode::ToolSetup))
            showPlaybackPanel();
    }

    void showSource() {
        stopPlayback();
        viewingSource = true;
        if (displayedPreview) {
            displayedPreview.reset();
            previewDisplayError = viewer->endBendPreview();
        }
    }

    void refreshWorkflow() {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        const auto preview = controller->bendPreview();
        const auto tools = toolSelection->parameters();
        const bool chosen =
            tools && (toolSelection->usesCatalog() || toolDraftChanged || (entry && entry->tools));
        partName->setText(document ? displayPath(document->metadata().sourcePath.filename())
                                   : QStringLiteral("Начните с CAD-модели"));
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
        partPrimary->setText(!document ? QStringLiteral("Открыть деталь…")
                             : preview ? QStringLiteral("Показать результат")
                             : !singleBendSupported || !matchingBendThickness
                                 ? QStringLiteral("Условия проверки")
                             : chosen ? QStringLiteral("Проверить гибку")
                                      : QStringLiteral("Выбрать инструмент →"));
        const auto bendCount = document ? document->metadata().bendAnalysis.bends.size() : 0;
        processSteps[0]->setStatus(document ? QStringLiteral("Найдено сгибов: %1").arg(bendCount)
                                            : QStringLiteral("Открыть STEP"));
        processSteps[1]->setStatus(chosen && document ? toolSelection->shortSummary()
                                                      : QStringLiteral("Выбрать пару"));
        processSteps[2]->setStatus(
            controller->isPreparingPreview() ? QStringLiteral("Проверяем…")
            : !document                      ? QStringLiteral("Нужна деталь")
            : !singleBendSupported ? QStringLiteral("Пока не поддерживается")
            : !matchingBendThickness || !entry || !included(entry->review)
                ? QStringLiteral("Нужно уточнение")
            : controller->bendPreviewError() || !previewDisplayError.isEmpty()
                ? QStringLiteral("Есть проблема")
            : preview && preview->pressMotion() ? QStringLiteral("Проверено в модели")
            : preview ? QStringLiteral("Просмотр подготовлен")
            : !chosen ? QStringLiteral("Нужен инструмент")
                      : QStringLiteral("Нужна проверка"));
        if (auto *dock = window->findChild<QDockWidget *>(QStringLiteral("sequenceDock")))
            dock->setVisible(propertyTabs->currentIndex() == 2 && !viewingSource &&
                             (preview || controller->isPreparingPreview()));
    }

    void showPlaybackPanel() {
        if (auto *dock = window->findChild<QDockWidget *>(QStringLiteral("sequenceDock"))) {
            dock->show();
            dock->raise();
        }
    }
    void stopPlayback() {
        playback->stop();
        playBend->setText(QStringLiteral("Воспроизвести"));
    }
    void showPreviewFrame() {
        const auto preview = controller->bendPreview();
        if (!preview || viewingSource) {
            return;
        }
        const auto index = static_cast<std::size_t>(bendFrame->value());
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
        previewDisplayError.clear();
        const double progressValue =
            preview->frameCount() > 1
                ? static_cast<double>(index) / static_cast<double>(preview->frameCount() - 1)
                : 0.0;
        const auto approximation =
            preview->ignoresClosedCutouts() ? QStringLiteral("Проверка без вырезов · ") : QString{};
        if (const auto *state = preview->pressState(index)) {
            frameLabel->setText(
                approximation +
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
        frameLabel->setText(approximation +
                            (preview->toolSetup()
                                 ? QStringLiteral("Начальное положение · статичный просмотр")
                                 : QStringLiteral("%1% · поворот %2°")
                                       .arg(number(progressValue * 100.0),
                                            number(progressValue * preview->model().angleRadians *
                                                   180.0 / std::numbers::pi))));
    }
    void refreshPreview() {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        const bool hasInteriorContours =
            document && !document->metadata().bendAnalysis.interiorContourDiagnostics.empty() &&
            document->metadata().closedCutouts.status != ClosedCutoutStatus::Filled;
        bool supported = document && !hasInteriorContours && row == 0 &&
                         document->metadata().bendAnalysis.bends.size() == 1 &&
                         !document->metadata().bendAnalysis.bends[0].trimmedEnds;
        bool matchingThickness = true;
        if (supported && session.manualThicknessMm()) {
            matchingThickness = std::abs(*session.manualThicknessMm() -
                                         document->metadata().bendAnalysis.bends[0].thicknessMm) <=
                                document->metadata().bendAnalysis.lengthToleranceMm;
        }
        preparePreview->setEnabled(
            previewBuilder && supported && matchingThickness && entry && included(entry->review) &&
            session.bendTarget(static_cast<std::size_t>(row)) && !controller->hasWork());
        fixedSide->setEnabled(document && !controller->hasWork());
        toolMode->setEnabled(document && !controller->hasWork());
        const bool toolEditable = entry && included(entry->review) && !controller->hasWork();
        toolSelection->setEnabled(toolEditable);
        applyTools->setEnabled(toolEditable && previewBuilder && supported && matchingThickness &&
                               session.bendTarget(static_cast<std::size_t>(row)).has_value());
        checkBend->setEnabled(applyTools->isEnabled());
        clearTools->setEnabled(toolEditable && entry->tools.has_value());
        const auto entryId = entry ? entry->id : std::string{};
        const auto savedTools = entry ? entry->tools : std::nullopt;
        const auto savedAssignment = entry ? entry->catalogTools : std::nullopt;
        if (toolsDocumentRevision != session.revision() || toolsEntryId != entryId ||
            shownTools != savedTools || shownAssignment != savedAssignment) {
            toolsDocumentRevision = session.revision();
            toolsEntryId = entryId;
            shownTools = savedTools;
            shownAssignment = savedAssignment;
            toolDraftChanged = false;
            toolSelection->setChoice(savedTools, savedAssignment);
        }
        const auto preview = controller->bendPreview();
        if (!preview) {
            previewDisplayError.clear();
            positionedPreview.reset();
        }
        if (!preview && displayedPreview) {
            stopPlayback();
            displayedPreview.reset();
            const auto failure = viewer->endBendPreview();
            if (!failure.isEmpty()) {
                previewStatus->setToolTip(failure);
            }
        }
        if (preview && displayedPreview != preview && !viewingSource) {
            if (positionedPreview != preview) {
                const QSignalBlocker block(bendFrame);
                bendFrame->setRange(0, static_cast<int>(preview->frameCount() - 1));
                bendFrame->setValue(0);
                positionedPreview = preview;
            }
            showPreviewFrame();
        }
        const bool staticSetup = preview && preview->toolSetup() && !preview->pressMotion();
        previewControls->setCurrentIndex(staticSetup ? 1 : 0);
        playBend->setVisible(!staticSetup);
        playbackSpeedControls->setVisible(!staticSetup);
        showKinematics->setVisible(staticSetup);
        showKinematics->setEnabled(staticSetup && preparePreview->isEnabled());
        bendFrame->setEnabled(preview && !viewingSource && !staticSetup && !controller->hasWork() &&
                              previewDisplayError.isEmpty());
        playBend->setEnabled(preview && !viewingSource && !staticSetup && !controller->hasWork() &&
                             previewDisplayError.isEmpty());
        playbackSpeedControls->setEnabled(playBend->isEnabled());
        returnToCad->setEnabled(preview || controller->isPreparingPreview());
        if (!preview || viewingSource || staticSetup || controller->hasWork()) {
            stopPlayback();
        }
        if (controller->isPreparingPreview()) {
            frameLabel->setText(QStringLiteral("Подготовка просмотра…"));
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
                : failure->code == BendPreviewErrorCode::SupportNotProven
                    ? QStringLiteral(
                          "Не удалось подтвердить сплошной участок опоры обеих полок: проверьте "
                          "отверстия/контур около сгиба и раскрытие V. Для движения участок "
                          "опоры проверяется на всём ходе.")
                : failure->code == BendPreviewErrorCode::ToolIntersection
                    ? QStringLiteral("Не удалось подтвердить отсутствие пересечения с инструментом "
                                     "в положении %1. Проверьте параметры.")
                          .arg(failure->frameIndex.value_or(0) + 1)
                : failure->code == BendPreviewErrorCode::ContactLost
                    ? QStringLiteral("Не удалось подтвердить рабочий контакт в положении %1. "
                                     "Просмотр движения не подготовлен.")
                          .arg(failure->frameIndex.value_or(0) + 1)
                : failure->code == BendPreviewErrorCode::RetractionNotProven
                    ? QStringLiteral(
                          "Не удалось подтвердить свободный отвод пуансона. "
                          "Полный цикл не подготовлен; проверьте инструмент и геометрию.")
                : failure->code == BendPreviewErrorCode::UnsupportedParameters
                    ? QStringLiteral("Параметры вне диапазона первого сценария: поворот 5–175°, "
                                     "толщина/радиус 0,1–100000 мм, ширина/полки 1–100000 мм.")
                : failure->code == BendPreviewErrorCode::BendZoneInteriorContours
                    ? QStringLiteral("В зоне сгиба обнаружены внутренние контуры отверстия или "
                                     "кармана. Эта геометрия пока не поддерживается для движения. "
                                     "Выберите диагностику D слева для подсветки.")
                : failure->code == BendPreviewErrorCode::UnsupportedGeometry
                    ? QStringLiteral(
                          "Геометрия не поддерживается первым сценарием. Нужна одна "
                          "целая прямая зона и две плоские полки в пределах ширины сгиба. "
                          "Отверстия и контур полок сохраняются; геометрия всей детали "
                          "должна пройти проверку восстановления.")
                    : QStringLiteral("Не удалось подготовить кинематический просмотр. Исходная "
                                     "деталь сохранена."));
            previewStatus->setToolTip(text(failure->detail));
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
                    "Заготовка (габарит): %1 × %2 × %3 мм\nГабарит неподвижной полки: %4 "
                    "мм\nГабарит подвижной полки: %5 "
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
                                   "Выберите единственный прямой сгиб детали с двумя "
                                   "плоскими полками. Отверстия и скошенные края полок допустимы. "
                                   "Многогибовые детали и подрезанные "
                                   "торцы пока поддерживаются только для просмотра исходного STEP.")
                : entry && !included(entry->review)
                    ? QStringLiteral("Зона исключена или ожидает решения из сохранённого проекта. "
                                     "Откройте параметры выбранного сгиба и выберите участие.")
                : !matchingThickness
                    ? QStringLiteral("Ручная толщина отличается от геометрии. Для просмотра "
                                     "используйте толщину из STEP.")
                    : QStringLiteral("Параметры сгиба взяты из CAD. Выберите инструмент и нажмите "
                                     "«Проверить и показать гибку»."));
            frameLabel->setText(QStringLiteral("Исходная CAD-форма"));
        }
        const auto outcome = previewStatus->text();
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
                QStringLiteral(
                    "\n\nИнструмент из каталога: проверена идеализированная рабочая зона. Полные "
                    "корпуса, поднутрения, держатели, станок и допустимая нагрузка не проверены."));
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
                ? QStringLiteral("Выберите пуансон, матрицу и рабочий ручей.")
                : QString{};
        const bool busy = controller->isPreparingPreview();
        const bool failed =
            controller->bendPreviewError().has_value() || !previewDisplayError.isEmpty();
        checkSummary->setText(
            busy ? QStringLiteral("Проверяем деталь и готовим движение…")
            : failed ? outcome
            : preview && preview->pressMotion()
                ? QStringLiteral("Геометрическая проверка пройдена.\nПодвод и гибка — по кадрам, "
                                 "вертикальный отвод — непрерывно.")
            : preview && preview->toolSetup()
                ? QStringLiteral("Начальная установка проверена.\nОдно неподвижное положение; "
                                 "движение не проверено.")
            : preview
                ? QStringLiteral("Просмотр без инструмента. Контакт и ход пресса не проверены.")
            : !checkBlocker.isEmpty() ? checkBlocker
            : toolDraftChanged
                ? QStringLiteral("Параметры изменены. Нужна новая проверка.")
                : QStringLiteral("Деталь и пара инструмента выбраны. Можно проверить гибку."));
        checkScope->setText(
            QStringLiteral("Упрощённые рабочие профили. Станок, держатели, усилие и пружинение "
                           "не проверяются.") +
            (document && document->metadata().closedCutouts.status == ClosedCutoutStatus::Filled
                 ? QStringLiteral(
                       "\nВырезы видимы; опоры и зазоры проверяются по заполненной копии. "
                       "Физика и реальная опора у вырезов не проверены.")
                 : QString{}));
        checkScope->setVisible(document != nullptr);
        checkBend->setEnabled(applyTools->isEnabled() && toolSelection->parameters().has_value());
        checkBend->setVisible(!preview);
        checkBend->setText(busy ? QStringLiteral("Проверяем…") : QStringLiteral("Проверить гибку"));
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
        resolveCheck->setText(
            checkAction == CheckAction::Tools ? QStringLiteral("Изменить инструмент…")
            : checkAction == CheckAction::Thickness ? QStringLiteral("Уточнить толщину…")
            : checkAction == CheckAction::Bend ? QStringLiteral("Уточнить сгиб…")
                                               : QStringLiteral("Посмотреть деталь →"));
        resolveCheck->setVisible(document && !busy && !preview &&
                                 (failed || !checkBlocker.isEmpty()));
        resolveCheck->setEnabled(!controller->hasWork());
        checkSummary->setToolTip(previewStatus->toolTip());
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
            pairs->addItem(QStringLiteral("Грани %1 и %2 · %3 мм")
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
    void save(bool choosePath) {
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
    viewer::ViewerWidget *viewer = nullptr;
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
    application::BendPreviewPtr displayedPreview;
    QString previewDisplayError;
    QComboBox *fixedSide = nullptr;
    QPushButton *preparePreview = nullptr;
    QLabel *previewStatus = nullptr;
    QLabel *checkSummary = nullptr;
    ToolSelectionWidget *toolSelection = nullptr;
    QPushButton *checkBend = nullptr;
    std::array<ProcessStepButton *, 3> processSteps{};
    QPushButton *partPrimary = nullptr;
    QPushButton *changePartTools = nullptr;
    QPushButton *editBend = nullptr;
    QLabel *partName = nullptr;
    QLabel *partToolSummary = nullptr;
    QLabel *partReadiness = nullptr;
    QLabel *checkScope = nullptr;
    QPushButton *resolveCheck = nullptr;
    QString checkBlocker;
    bool toolDraftChanged = false;
    bool singleBendSupported = false;
    bool matchingBendThickness = true;
    enum class CheckAction { Part, Tools, Bend, Thickness };
    CheckAction checkAction = CheckAction::Part;
    bool viewingSource = false;
    application::BendPreviewPtr positionedPreview;
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
    QWidget *playbackSpeedControls = nullptr;
    QSlider *playbackSpeed = nullptr;
    QLabel *playbackSpeedValue = nullptr;
    QPushButton *showKinematics = nullptr;
    QLabel *setupPlaybackHint = nullptr;
    QStackedWidget *previewControls = nullptr;
    QPushButton *returnToCad = nullptr;
    QLabel *frameLabel = nullptr;
    QTimer *playback = nullptr;
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
    QToolBar *camera = nullptr;
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
    setCentralWidget(impl_->viewer);
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
    impl_->cancelAction = fileMenu->addAction(QStringLiteral("Отменить загрузку"));
    impl_->cancelAction->setObjectName(QStringLiteral("cancelImportAction"));
    impl_->cancelAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(impl_->cancelAction, &QAction::triggered, impl_->controller, &ImportController::cancel);
    impl_->closeAction = fileMenu->addAction(QStringLiteral("Закрыть деталь"));
    impl_->closeAction->setObjectName(QStringLiteral("closeDocumentAction"));
    impl_->closeAction->setShortcut(QKeySequence::Close);
    connect(impl_->closeAction, &QAction::triggered, this, [this] {
        if (impl_->canDiscard()) {
            impl_->controller->closeDocument();
        }
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
    toolbar->addAction(impl_->saveAction);
    toolbar->addAction(impl_->cancelAction);
    auto *toolSetupAction = viewMenu->addAction(QStringLiteral("Инструмент"));
    toolSetupAction->setObjectName(QStringLiteral("toolSetupAction"));
    connect(toolSetupAction, &QAction::triggered, this, [this] {
        impl_->propertyTabs->setCurrentIndex(3);
        impl_->propertyTabs->parentWidget()->show();
    });
    impl_->camera = addToolBar(QStringLiteral("Камера"));
    impl_->camera->setObjectName(QStringLiteral("cameraToolbar"));
    impl_->camera->setMovable(false);
    auto *fit = impl_->camera->addAction(QStringLiteral("Вписать · F"));
    auto *views = new QMenu(QStringLiteral("Ракурс"), impl_->camera);
    auto *viewButton = new QToolButton(impl_->camera);
    viewButton->setText(QStringLiteral("Ракурс"));
    viewButton->setMenu(views);
    viewButton->setPopupMode(QToolButton::InstantPopup);
    impl_->camera->addWidget(viewButton);
    connect(fit, &QAction::triggered, impl_->viewer, &viewer::ViewerWidget::fitAll);
    const auto orientation = [&](const QString &label, viewer::ViewerWidget::Orientation value) {
        auto *action = views->addAction(label);
        connect(action, &QAction::triggered, impl_->viewer,
                [this, value] { impl_->viewer->setOrientation(value); });
    };
    orientation(QStringLiteral("Изометрия"), viewer::ViewerWidget::Orientation::Isometric);
    orientation(QStringLiteral("Сверху"), viewer::ViewerWidget::Orientation::Top);
    orientation(QStringLiteral("Спереди"), viewer::ViewerWidget::Orientation::Front);

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
        if (impl_->controller->bendPreview() || impl_->controller->isPreparingPreview()) {
            impl_->controller->discardBendPreview();
        }
        impl_->showSelectedBend(true);
        impl_->refreshPreview();
    });
    impl_->selection = label(QStringLiteral("faceProperties"));
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
        } else if (!impl_->singleBendSupported || !impl_->matchingBendThickness) {
            impl_->propertyTabs->setCurrentIndex(2);
        } else if (impl_->controller->bendPreview()) {
            impl_->viewingSource = false;
            impl_->propertyTabs->setCurrentIndex(2);
            impl_->refreshPreview();
        } else if (impl_->toolSelection->parameters() &&
                   (impl_->toolSelection->usesCatalog() || impl_->toolDraftChanged ||
                    impl_->shownTools)) {
            impl_->toolMode->setCurrentIndex(0);
            impl_->checkTools();
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
    auto *operationTitle = new QLabel(QStringLiteral("Проверка и симуляция гибки"), operationPanel);
    operationTitle->setWordWrap(true);
    operationLayout->addWidget(operationTitle);
    impl_->checkBend = new QPushButton(QStringLiteral("Проверить гибку"), operationPanel);
    impl_->checkBend->setObjectName(QStringLiteral("checkBend"));
    impl_->checkBend->setMinimumHeight(40);

    impl_->fixedSide = new QComboBox(operationPanel);
    impl_->fixedSide->setObjectName(QStringLiteral("fixedBendSide"));
    impl_->fixedSide->setAccessibleName(QStringLiteral("Неподвижная полка сгиба"));
    impl_->fixedSide->addItems({QStringLiteral("Полка A"), QStringLiteral("Полка B")});

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
    operationLayout->addWidget(impl_->previewStatus);
    impl_->previewStatus->hide();
    connect(reportToggle, &QToolButton::toggled, impl_->previewStatus, &QWidget::setVisible);
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
    advancedToggle->setText(QStringLiteral("Движение без инструмента ▸"));
    advancedToggle->setCheckable(true);
    advancedToggle->setMinimumHeight(32);
    auto *advanced = new QWidget(operationPanel);
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->addWidget(impl_->fixedSide);
    advancedLayout->addWidget(impl_->preparePreview);
    advancedLayout->addWidget(kinematicHint);
    operationLayout->addWidget(advancedToggle);
    operationLayout->addWidget(advanced);
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
    impl_->applyTools = new QPushButton(QStringLiteral("Проверить гибку"), toolPanel);
    impl_->applyTools->setObjectName(QStringLiteral("applyToolSetup"));
    impl_->applyTools->setMinimumHeight(40);

    impl_->toolSelection = new ToolSelectionWidget(std::move(catalog), toolPanel);
    toolLayout->addWidget(impl_->toolSelection);
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
    connect(impl_->toolMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        impl_->applyTools->setText(index == 0 ? QStringLiteral("Проверить гибку")
                                              : QStringLiteral("Проверить начальную установку"));
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
    connect(impl_->applyTools, &QPushButton::clicked, this, [this] { impl_->checkTools(); });
    connect(impl_->checkBend, &QPushButton::clicked, this, [this] {
        impl_->toolMode->setCurrentIndex(0);
        impl_->checkTools();
    });
    impl_->toolSelection->onChanged = [this] {
        impl_->toolDraftChanged = true;
        impl_->controller->discardBendPreview();
        impl_->refreshPreview();
    };
    connect(impl_->clearTools, &QPushButton::clicked, this, [this] {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0) {
            impl_->controller->setBendTools(static_cast<std::size_t>(row), std::nullopt);
        }
    });
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Текущая задача"), impl_->propertyTabs,
            Qt::RightDockWidgetArea);
    impl_->propertyTabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane{border:0;background:white;}QScrollArea{border:0;background:white;}"));
    resizeDocks({findChild<QDockWidget *>(QStringLiteral("propertiesDock"))}, {350},
                Qt::Horizontal);
    auto *sequence = new QWidget(this);
    auto *sequenceLayout = new QVBoxLayout(sequence);
    impl_->frameLabel = new QLabel(sequence);
    impl_->frameLabel->setObjectName(QStringLiteral("bendFrameLabel"));
    sequenceLayout->addWidget(impl_->frameLabel);
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
    impl_->returnToCad = new QPushButton(QStringLiteral("Вернуться к STEP"), sequence);
    impl_->returnToCad->setObjectName(QStringLiteral("returnToCad"));
    playLayout->addWidget(impl_->playBend);
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
    impl_->playbackSpeed->setMaximumWidth(160);
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
        if (impl_->bendFrame->value() >= impl_->bendFrame->maximum()) {
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
        if (impl_->bendFrame->value() == impl_->bendFrame->maximum()) {
            impl_->bendFrame->setValue(0);
        }
        impl_->playBend->setText(QStringLiteral("Пауза"));
        impl_->playback->start();
    });
    connect(impl_->returnToCad, &QPushButton::clicked, this, [this] {
        impl_->showSource();
        impl_->propertyTabs->setCurrentIndex(0);
        impl_->refreshPreview();
    });
    connect(impl_->fixedSide, &QComboBox::currentIndexChanged, impl_->controller,
            &ImportController::discardBendPreview);
    connect(impl_->preparePreview, &QPushButton::clicked, this, [this] {
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
    addDock(QStringLiteral("sequenceDock"), QStringLiteral("Просмотр операции"), sequence,
            Qt::BottomDockWidgetArea);

    findChild<QDockWidget *>(QStringLiteral("sequenceDock"))->hide();
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
                                            QStringLiteral("Проверка")};
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
            else if (i == 2)
                impl_->viewingSource = false;
            impl_->propertyTabs->setCurrentIndex(stageTabs[static_cast<std::size_t>(i)]);
            impl_->propertyTabs->parentWidget()->show();
            impl_->refreshPreview();
        });
    }
    processGroup->button(0)->setChecked(true);
    connect(impl_->propertyTabs, &QTabWidget::currentChanged, this, [this, processGroup](int page) {
        const int stage = page == 3 ? 1 : page == 2 ? 2 : 0;
        processGroup->button(stage)->setChecked(true);
        if (page != 2)
            impl_->stopPlayback();
        impl_->refreshWorkflow();
    });
    processBar->addWidget(processPanel);

    impl_->progress = new QProgressBar(this);
    impl_->progress->setObjectName(QStringLiteral("importProgress"));
    impl_->progress->setRange(0, 0);
    impl_->progress->setMaximumWidth(140);
    impl_->progress->setAccessibleName(QStringLiteral("Загрузка STEP"));
    statusBar()->addPermanentWidget(impl_->progress);

    connect(impl_->controller, &ImportController::stateChanged, this, [this] { impl_->refresh(); });
    connect(impl_->viewer, &viewer::ViewerWidget::renderingChanged, this,
            [this](bool) { impl_->refresh(); });
    connect(impl_->viewer, &viewer::ViewerWidget::selectionChanged, this,
            [this](quint64 revision, int index) {
                const auto &session = impl_->controller->session();
                if (revision == session.revision() && session.document() && index > 0 &&
                    static_cast<std::size_t>(index) <= session.document()->faces().size()) {
                    const auto &face =
                        session.document()->faces()[static_cast<std::size_t>(index - 1)];
                    impl_->selection->setText(
                        QStringLiteral("Грань №%1\nПоверхность: %2\nПлощадь: %3 мм²")
                            .arg(face.index)
                            .arg(surfaceName(face.surface), number(face.areaMm2)));
                } else {
                    impl_->selection->setText(
                        QStringLiteral("Грань не выбрана. Нажмите ЛКМ на модели."));
                }
            });

    auto *help = menuBar()->addMenu(QStringLiteral("Справка"));
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
}

MainWindow::~MainWindow() {
    impl_->controller->shutdown();
}
void MainWindow::openStep(const QString &path) {
    if (!path.isEmpty() && impl_->canDiscard()) {
        impl_->controller->open(nativePath(path));
    }
}
ImportController &MainWindow::importController() const {
    return *impl_->controller;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!impl_->canDiscard()) {
        event->ignore();
        return;
    }
    impl_->controller->closeDocument();
    QMainWindow::closeEvent(event);
}
} // namespace bending::ui
