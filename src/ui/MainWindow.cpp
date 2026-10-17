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
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
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
        if (!shownEditRevision || *shownEditRevision != session.editRevision()) {
            shownEditRevision = session.editRevision();
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
        showSelectedBend(false);
        camera->setEnabled(viewer->isReady() && document != nullptr);
        closeAction->setEnabled(document != nullptr || session.activeRequest().has_value());
        cancelAction->setEnabled(session.activeRequest().has_value());
        progress->setVisible(session.activeRequest().has_value() || controller->isSaving());
        if (controller->isSaving()) {
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
            item->setEditable(false);
            bendModel->appendRow(item);
        }
    }
    void showSelectedBend(bool highlight) {
        const auto &session = controller->session();
        const auto &document = session.document();
        const auto row = bendList->currentIndex().row();
        if (!document || row < 0 ||
            static_cast<std::size_t>(row) >= document->metadata().bendAnalysis.bends.size()) {
            bendProperties->setText(QStringLiteral(
                "Выберите найденный сгиб слева, чтобы посмотреть параметры и поверхности."));
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
                "Это параметры загруженной формы. Операция гибки ещё не задана.")
                .arg(row + 1, 2, 10, QLatin1Char('0'))
                .arg(number(bend.angleRadians * 180.0 / std::numbers::pi),
                     number(bend.innerRadiusMm), number(bend.thicknessMm), number(bend.lengthMm),
                     coordinate(bend.axisStart.x), coordinate(bend.axisStart.y),
                     coordinate(bend.axisStart.z), coordinate(bend.axisEnd.x),
                     coordinate(bend.axisEnd.y), coordinate(bend.axisEnd.z));
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
        if (highlight && !controller->hasWork()) {
            std::vector<std::size_t> indices{bend.innerFace, bend.outerFace};
            indices.insert(indices.end(), bend.flangeFaces.begin(), bend.flangeFaces.end());
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
            viewer->highlightFaces(indices, session.revision());
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
    std::optional<std::uint64_t> shownEditRevision;
};

MainWindow::MainWindow(RuntimeInfo runtime,
                       std::shared_ptr<const application::DocumentImporter> importer,
                       std::shared_ptr<const application::ProjectWriter> writer)
    : impl_(std::make_unique<Impl>(this)) {
    impl_->writer = std::move(writer);
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
    impl_->bendSummary->setWordWrap(true);
    impl_->bendSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    connect(impl_->bendList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { impl_->showSelectedBend(true); });
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
    impl_->bendProperties = new QLabel(this);
    impl_->bendProperties->setObjectName(QStringLiteral("bendProperties"));
    impl_->bendProperties->setWordWrap(true);
    impl_->bendProperties->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    impl_->bendProperties->setTextFormat(Qt::PlainText);
    impl_->bendProperties->setTextInteractionFlags(Qt::TextSelectableByMouse);
    impl_->bendProperties->setMargin(16);
    auto *bendScroll = new QScrollArea(this);
    bendScroll->setWidgetResizable(true);
    bendScroll->setWidget(impl_->bendProperties);
    impl_->propertyTabs = new QTabWidget(this);
    impl_->propertyTabs->setObjectName(QStringLiteral("propertiesTabs"));
    impl_->propertyTabs->addTab(scroll, QStringLiteral("Деталь"));
    impl_->propertyTabs->addTab(bendScroll, QStringLiteral("Сгиб"));
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Параметры"), impl_->propertyTabs,
            Qt::RightDockWidgetArea);
    auto *sequence =
        new QLabel(QStringLiteral("Ручная последовательность · операции ещё не заданы"), this);
    sequence->setMargin(16);
    sequence->setMinimumHeight(100);
    addDock(QStringLiteral("sequenceDock"), QStringLiteral("Порядок гибов"), sequence,
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
            impl_->title + QStringLiteral("\n\nИмпорт и просмотр STEP одной детали.\nМоделирование "
                                          "гибки и столкновения ещё не реализованы."));
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
