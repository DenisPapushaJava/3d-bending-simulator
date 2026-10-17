#include "bending/ui/MainWindow.h"

#include "bending/ui/ImportController.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
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
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
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
        if (shownBendRevision != session.revision()) {
            shownBendRevision = session.revision();
            fillBends();
        }
        if (shownThicknessRevision != session.revision() ||
            shownManualThickness != session.manualThicknessMm()) {
            shownThicknessRevision = session.revision();
            shownManualThickness = session.manualThicknessMm();
            fillThicknessControls();
        }
        const bool editable = document && !controller->hasWork();
        thicknessInput->setEnabled(editable);
        applyThickness->setEnabled(editable && thicknessInput->value() > 0.0);
        automaticThickness->setEnabled(editable && session.manualThicknessMm().has_value());
        pairs->setEnabled(editable && pairs->count() > 0);
        showPair->setEnabled(editable && viewer->isReady() && pairs->count() > 0);
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
        cancelAction->setEnabled(session.activeRequest().has_value() ||
                                 controller->isPreparingPreview());
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
                "Кинематический просмотр · ПКМ: вращение · СКМ: перенос · Колесо: масштаб"));
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

    void stopPlayback() {
        playback->stop();
        playBend->setText(QStringLiteral("Воспроизвести"));
    }
    void showPreviewFrame() {
        const auto preview = controller->bendPreview();
        if (!preview) {
            return;
        }
        const auto index = static_cast<std::size_t>(bendFrame->value());
        const auto error =
            viewer->presentBendPreview(preview, index, controller->session().revision());
        if (!error.isEmpty()) {
            previewDisplayError = error;
            stopPlayback();
            previewStatus->setText(QStringLiteral("Не удалось показать состояние: %1").arg(error));
            return;
        }
        displayedPreview = preview;
        previewDisplayError.clear();
        const double progressValue =
            static_cast<double>(index) / static_cast<double>(preview->frameCount() - 1);
        frameLabel->setText(QStringLiteral("%1% · поворот %2°")
                                .arg(number(progressValue * 100.0),
                                     number(progressValue * preview->model().angleRadians * 180.0 /
                                            std::numbers::pi)));
    }
    void refreshPreview() {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        const auto *entry = row < 0 ? nullptr : session.bend(static_cast<std::size_t>(row));
        bool supported = document && row == 0 &&
                         document->metadata().bendAnalysis.bends.size() == 1 &&
                         !document->metadata().bendAnalysis.bends[0].trimmedEnds &&
                         document->metadata().faceCount == 10;
        bool matchingThickness = true;
        if (supported && session.manualThicknessMm()) {
            matchingThickness = std::abs(*session.manualThicknessMm() -
                                         document->metadata().bendAnalysis.bends[0].thicknessMm) <=
                                document->metadata().bendAnalysis.lengthToleranceMm;
        }
        preparePreview->setEnabled(previewBuilder && supported && matchingThickness && entry &&
                                   entry->review == BendReview::Confirmed && entry->target &&
                                   !controller->hasWork());
        fixedSide->setEnabled(document && !controller->hasWork());
        const auto preview = controller->bendPreview();
        if (!preview) {
            previewDisplayError.clear();
        }
        if (!preview && displayedPreview) {
            stopPlayback();
            displayedPreview.reset();
            const auto error = viewer->endBendPreview();
            if (!error.isEmpty()) {
                previewStatus->setToolTip(error);
            }
        }
        if (preview && displayedPreview != preview) {
            const QSignalBlocker block(bendFrame);
            bendFrame->setRange(0, static_cast<int>(preview->frameCount() - 1));
            bendFrame->setValue(0);
            showPreviewFrame();
        }
        bendFrame->setEnabled(preview && !controller->hasWork() && previewDisplayError.isEmpty());
        playBend->setEnabled(preview && !controller->hasWork() && previewDisplayError.isEmpty());
        returnToCad->setEnabled(preview || controller->isPreparingPreview());
        if (!preview || controller->hasWork()) {
            stopPlayback();
        }
        if (controller->isPreparingPreview()) {
            previewStatus->setText(
                QStringLiteral("Проверяем восстановление всей детали и готовим 21 состояние…"));
        } else if (const auto &error = controller->bendPreviewError()) {
            previewStatus->setText(
                error->code == BendPreviewErrorCode::TargetMismatch
                    ? QStringLiteral("Для первого просмотра целевые угол и радиус должны совпадать "
                                     "с STEP. Верните параметры из CAD.")
                : error->code == BendPreviewErrorCode::UnsupportedParameters
                    ? QStringLiteral("Параметры вне диапазона первого сценария: поворот 5–175°, "
                                     "толщина/радиус 0,1–100000 мм, ширина/полки 1–100000 мм.")
                : error->code == BendPreviewErrorCode::UnsupportedGeometry
                    ? QStringLiteral("Геометрия не поддерживается первым сценарием. Нужна одна "
                                     "прямая зона и две прямоугольные полки без вырезов/подрезки; "
                                     "исходная деталь доступна для просмотра.")
                    : QStringLiteral("Не удалось подготовить кинематический просмотр. Исходная "
                                     "деталь сохранена."));
            previewStatus->setToolTip(text(error->detail));
        } else if (!previewDisplayError.isEmpty()) {
            previewStatus->setText(
                QStringLiteral("Не удалось показать состояние: %1").arg(previewDisplayError));
        } else if (preview) {
            const auto &m = preview->model();
            previewStatus->setText(
                QStringLiteral(
                    "Заготовка: %1 × %2 × %3 мм\nНеподвижная полка: %4 мм\nПодвижная полка: %5 "
                    "мм\nДуга средней поверхности: %6 мм\n\nСиняя — неподвижная полка; оранжевая — "
                    "зона сгиба; зелёная — подвижная полка.")
                    .arg(number(simulation::blankLengthMm(m)), number(m.widthMm),
                         number(m.thicknessMm), number(m.fixedFlangeMm), number(m.movingFlangeMm),
                         number(simulation::bendAllowanceMm(m))));
            previewStatus->setToolTip({});
        } else {
            previewStatus->setText(
                !document ? QStringLiteral("Откройте учебную деталь bend-90-6mm.step.")
                : !supported ? QStringLiteral(
                                   "Выберите единственный прямой сгиб детали с двумя "
                                   "прямоугольными полками. Многогибовые детали и подрезанные "
                                   "торцы пока поддерживаются только для просмотра исходного STEP.")
                : !matchingThickness
                    ? QStringLiteral("Ручная толщина отличается от геометрии. Для просмотра "
                                     "используйте толщину из STEP.")
                    : QStringLiteral("Подтвердите сгиб на вкладке «Сгиб» и примените целевые "
                                     "параметры из CAD, затем подготовьте просмотр."));
            frameLabel->setText(QStringLiteral("Исходная CAD-форма"));
        }
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
        showThicknessEvidence(false);
    }
    void fillBends() {
        bendModel->clear();
        const auto &document = controller->session().document();
        if (!document) {
            bendSummary->setText(QStringLiteral("Сгибы: откройте деталь"));
            return;
        }
        const auto &analysis = document->metadata().bendAnalysis;
        switch (analysis.status) {
        case BendRecognitionStatus::Completed:
            bendSummary->setText(
                QStringLiteral("Найдено зон сгиба: %1\nЛокальный анализ прямых цилиндрических "
                               "сгибов. Полнота распознавания и технологичность не проверены.")
                    .arg(analysis.bends.size()));
            if (analysis.unrecognizedCylindricalFaces > 0) {
                bendSummary->setText(
                    bendSummary->text() +
                    QStringLiteral("\nЦилиндрических граней вне найденных зон: %1.")
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
        if (!controller->session().bendBindings().unresolved.empty()) {
            bendSummary->setText(
                bendSummary->text() +
                QStringLiteral(
                    "\nНе привязано сохранённых записей: %1. Подробности на вкладке «Сгиб».")
                    .arg(controller->session().bendBindings().unresolved.size()));
        }
        for (std::size_t i = 0; i < analysis.bends.size(); ++i) {
            const auto &bend = analysis.bends[i];
            auto *item =
                new QStandardItem(QStringLiteral("B%1 · %2° · R%3 · L%4 мм")
                                      .arg(i + 1, 2, 10, QLatin1Char('0'))
                                      .arg(number(bend.angleRadians * 180.0 / std::numbers::pi),
                                           number(bend.innerRadiusMm), number(bend.lengthMm)));
            if (bend.trimmedEnds) {
                item->setText(item->text() + QStringLiteral(" · подрезка"));
            }
            item->setToolTip(item->text());
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
            const auto status =
                entry ? reviewText(entry->review) : QStringLiteral("Нет однозначной привязки");
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
        const bool editable =
            entry && entry->review == BendReview::Confirmed && !controller->hasWork();
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
                           entry->review == BendReview::Confirmed ? QString{}
                                                                  : QStringLiteral(" · не активно"))
                : QStringLiteral("Задание не задано. В полях предложены измерения CAD."));
        updateTargetButtons();
        if (highlight && !controller->hasWork()) {
            viewer->highlightBend(bend, session.revision());
            propertyTabs->setCurrentIndex(1);
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
    QSlider *bendFrame = nullptr;
    QPushButton *playBend = nullptr;
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
                       std::shared_ptr<const application::SingleBendPreviewBuilder> previewBuilder)
    : impl_(std::make_unique<Impl>(this)) {
    impl_->writer = std::move(writer);
    impl_->previewBuilder = std::move(previewBuilder);
    setObjectName(QStringLiteral("mainWindow"));
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
    toolbar->addAction(impl_->closeAction);
    toolbar->addSeparator();
    for (const auto *label : {"Деталь", "Инструмент", "Порядок", "Проверка"}) {
        auto *action = toolbar->addAction(QString::fromUtf8(label));
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Запланировано для следующих этапов"));
    }
    impl_->camera = addToolBar(QStringLiteral("Камера"));
    impl_->camera->setObjectName(QStringLiteral("cameraToolbar"));
    auto *fit = impl_->camera->addAction(QStringLiteral("Вписать · F"));
    connect(fit, &QAction::triggered, impl_->viewer, &viewer::ViewerWidget::fitAll);
    const auto orientation = [&](const QString &label, viewer::ViewerWidget::Orientation value) {
        auto *action = impl_->camera->addAction(label);
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
        addDockWidget(area, dock);
        viewMenu->addAction(dock->toggleViewAction());
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
    impl_->bendList = new QListView(leftPanel);
    impl_->bendList->setObjectName(QStringLiteral("bendList"));
    impl_->bendList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    impl_->bendList->setAccessibleName(QStringLiteral("Распознанные зоны сгибов"));
    impl_->bendModel = new QStandardItemModel(impl_->bendList);
    impl_->bendList->setModel(impl_->bendModel);
    leftLayout->addWidget(impl_->bendList);
    addDock(QStringLiteral("projectDock"), QStringLiteral("Проект и сгибы"), leftPanel,
            Qt::LeftDockWidgetArea);

    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 16, 16, 16);
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
    impl_->error = label(QStringLiteral("importError"));
    impl_->error->setStyleSheet(QStringLiteral("color:#9D2828;"));
    impl_->diagnostics = new QPlainTextEdit(panel);
    impl_->diagnostics->setObjectName(QStringLiteral("importDiagnostics"));
    impl_->diagnostics->setReadOnly(true);
    impl_->diagnostics->setMaximumHeight(150);
    impl_->diagnostics->setAccessibleName(QStringLiteral("Подробности диагностики импорта"));
    layout->addWidget(impl_->diagnostics);
    layout->addStretch();
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(300);
    scroll->setWidget(panel);
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
    for (const auto review : {BendReview::Pending, BendReview::Confirmed, BendReview::Excluded}) {
        impl_->bendReview->addItem(reviewText(review));
    }
    bendLayout->addWidget(impl_->bendReview);
    connect(impl_->bendReview, &QComboBox::activated, this, [this](int index) {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0 && index >= 0 && index <= 2) {
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
                       "Подтверждение зоны разрешает применить задание. Инструмент, направление, "
                       "развёртка и движение будут заданы на следующем этапе."),
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
    impl_->propertyTabs->addTab(scroll, QStringLiteral("Деталь"));
    impl_->propertyTabs->addTab(bendScroll, QStringLiteral("Сгиб"));
    auto *operationPanel = new QWidget(this);
    auto *operationLayout = new QVBoxLayout(operationPanel);
    auto *operationTitle =
        new QLabel(QStringLiteral("Один сгиб · кинематический просмотр"), operationPanel);
    operationTitle->setWordWrap(true);
    operationLayout->addWidget(operationTitle);
    operationLayout->addWidget(new QLabel(QStringLiteral("Неподвижная полка:"), operationPanel));
    impl_->fixedSide = new QComboBox(operationPanel);
    impl_->fixedSide->setObjectName(QStringLiteral("fixedBendSide"));
    impl_->fixedSide->setAccessibleName(QStringLiteral("Неподвижная полка сгиба"));
    impl_->fixedSide->addItems({QStringLiteral("Полка A"), QStringLiteral("Полка B")});
    operationLayout->addWidget(impl_->fixedSide);
    impl_->preparePreview = new QPushButton(QStringLiteral("Подготовить просмотр"), operationPanel);
    impl_->preparePreview->setObjectName(QStringLiteral("prepareBendPreview"));
    operationLayout->addWidget(impl_->preparePreview);
    impl_->previewStatus = new QLabel(operationPanel);
    impl_->previewStatus->setObjectName(QStringLiteral("bendPreviewStatus"));
    impl_->previewStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->previewStatus->setWordWrap(true);
    impl_->previewStatus->setTextFormat(Qt::PlainText);
    impl_->previewStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    operationLayout->addWidget(impl_->previewStatus);
    auto *kinematicHint = new QLabel(
        QStringLiteral("Модель сохраняет толщину и длину средней поверхности. Это геометрическая "
                       "кинематика.\n\n"
                       "Инструмент, контакты, столкновения, усилие и пружинение не рассчитываются. "
                       "Это не технологическая развёртка для производства.\n\nВыбор полки и кадра "
                       "относится к просмотру и не сохраняется в проекте."),
        operationPanel);
    kinematicHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    kinematicHint->setWordWrap(true);
    operationLayout->addWidget(kinematicHint);
    operationLayout->addStretch();
    auto *operationScroll = new QScrollArea(this);
    operationScroll->setWidgetResizable(true);
    operationScroll->setMinimumWidth(300);
    operationScroll->setWidget(operationPanel);
    impl_->propertyTabs->addTab(operationScroll, QStringLiteral("Операция"));
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Параметры"), impl_->propertyTabs,
            Qt::RightDockWidgetArea);
    auto *sequence = new QWidget(this);
    auto *sequenceLayout = new QVBoxLayout(sequence);
    impl_->frameLabel = new QLabel(sequence);
    impl_->frameLabel->setObjectName(QStringLiteral("bendFrameLabel"));
    sequenceLayout->addWidget(impl_->frameLabel);
    impl_->bendFrame = new QSlider(Qt::Horizontal, sequence);
    impl_->bendFrame->setObjectName(QStringLiteral("bendFrame"));
    impl_->bendFrame->setAccessibleName(QStringLiteral("Промежуточное состояние сгиба"));
    impl_->bendFrame->setRange(0, 20);
    impl_->bendFrame->setTickPosition(QSlider::TicksBelow);
    impl_->bendFrame->setTickInterval(5);
    sequenceLayout->addWidget(impl_->bendFrame);
    auto *playLayout = new QHBoxLayout;
    impl_->playBend = new QPushButton(QStringLiteral("Воспроизвести"), sequence);
    impl_->playBend->setObjectName(QStringLiteral("playBend"));
    impl_->returnToCad = new QPushButton(QStringLiteral("Вернуться к STEP"), sequence);
    impl_->returnToCad->setObjectName(QStringLiteral("returnToCad"));
    playLayout->addWidget(impl_->playBend);
    playLayout->addWidget(impl_->returnToCad);
    playLayout->addStretch();
    sequenceLayout->addLayout(playLayout);
    impl_->playback = new QTimer(this);
    impl_->playback->setInterval(150);
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
    connect(impl_->returnToCad, &QPushButton::clicked, impl_->controller,
            &ImportController::discardBendPreview);
    connect(impl_->fixedSide, &QComboBox::currentIndexChanged, impl_->controller,
            &ImportController::discardBendPreview);
    connect(impl_->preparePreview, &QPushButton::clicked, this, [this] {
        const auto row = impl_->bendList->currentIndex().row();
        if (row >= 0) {
            impl_->propertyTabs->setCurrentIndex(2);
            impl_->controller->prepareBendPreview(static_cast<std::size_t>(row),
                                                  impl_->fixedSide->currentIndex() == 1,
                                                  impl_->previewBuilder);
        }
    });
    addDock(QStringLiteral("sequenceDock"), QStringLiteral("Просмотр операции"), sequence,
            Qt::BottomDockWidgetArea);

    impl_->progress = new QProgressBar(this);
    impl_->progress->setObjectName(QStringLiteral("importProgress"));
    impl_->progress->setRange(0, 0);
    impl_->progress->setMaximumWidth(140);
    impl_->progress->setAccessibleName(QStringLiteral("Загрузка STEP"));
    statusBar()->addPermanentWidget(impl_->progress);
    statusBar()->addPermanentWidget(
        new QLabel(QStringLiteral("Qt %1 · OCCT %2")
                       .arg(QString::fromLatin1(qVersion()), text(runtime.cadVersion)),
                   this));
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
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("3D Bending Simulator"),
            impl_->title +
                QStringLiteral(
                    "\n\nИмпорт STEP и кинематический просмотр одной простой зоны сгиба.\nКонтакты "
                    "с инструментом и столкновения ещё не рассчитываются."));
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
