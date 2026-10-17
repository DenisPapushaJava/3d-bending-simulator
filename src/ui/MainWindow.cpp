#include "bending/ui/MainWindow.h"

#include "bending/ui/ImportController.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

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
    case ImportPhase::Preparing:
        return QStringLiteral("Подготовка 3D-модели");
    }
    return {};
}
QString errorText(ImportErrorCode code) {
    switch (code) {
    case ImportErrorCode::UnsupportedFormat:
        return QStringLiteral(
            "Выберите файл STEP (.step или .stp). Родные форматы CAD пока не поддерживаются.");
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
                project->addItem(QStringLiteral("Гибы ещё не заданы"));
                QStringList units;
                for (const auto &unit : metadata.sourceLengthUnits) {
                    units.append(text(unit));
                }
                const auto &bounds = metadata.bounds;
                properties->setText(
                    QStringLiteral(
                        "Файл: %1\n\nГабариты: %2 × %3 × %4 мм\nОбъём: %5 мм³\nГраней: "
                        "%6\n\nЕдиницы файла: %7\nВ программе: мм\nДопуск BRep (макс.): %8 "
                        "мм\n\nИсходная CAD-геометрия. Толщина и сгибы ещё не заданы.")
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
        camera->setEnabled(viewer->isReady() && document != nullptr);
        closeAction->setEnabled(document != nullptr || session.activeRequest().has_value());
        cancelAction->setEnabled(session.activeRequest().has_value());
        progress->setVisible(session.activeRequest().has_value());
        if (session.activeRequest()) {
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
        error->setVisible(session.error().has_value());
        QString detail;
        if (session.error()) {
            error->setText(errorText(session.error()->code));
            detail = text(session.error()->detail) + QLatin1Char('\n') +
                     diagnosticsText(session.error()->diagnostics);
        } else if (document) {
            detail = diagnosticsText(document->metadata().diagnostics);
        }
        diagnostics->setPlainText(detail);
        diagnostics->setVisible(!detail.isEmpty());
    }

    MainWindow *window;
    viewer::ViewerWidget *viewer = nullptr;
    ImportController *controller = nullptr;
    QListWidget *project = nullptr;
    QLabel *properties = nullptr;
    QLabel *selection = nullptr;
    QLabel *error = nullptr;
    QPlainTextEdit *diagnostics = nullptr;
    QToolBar *camera = nullptr;
    QAction *closeAction = nullptr;
    QAction *cancelAction = nullptr;
    QProgressBar *progress = nullptr;
    QString title;
    std::optional<std::uint64_t> shownRevision;
};

MainWindow::MainWindow(RuntimeInfo runtime,
                       std::shared_ptr<const application::DocumentImporter> importer)
    : impl_(std::make_unique<Impl>(this)) {
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
    auto *open = fileMenu->addAction(QStringLiteral("Открыть STEP…"));
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
            this, QStringLiteral("Открыть STEP одной детали"), folder,
            QStringLiteral("STEP (*.step *.stp *.STEP *.STP);;Все файлы (*)"));
        if (!path.isEmpty()) {
            openStep(path);
        }
    });
    impl_->cancelAction = fileMenu->addAction(QStringLiteral("Отменить загрузку"));
    impl_->cancelAction->setObjectName(QStringLiteral("cancelImportAction"));
    impl_->cancelAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(impl_->cancelAction, &QAction::triggered, impl_->controller, &ImportController::cancel);
    impl_->closeAction = fileMenu->addAction(QStringLiteral("Закрыть деталь"));
    impl_->closeAction->setObjectName(QStringLiteral("closeDocumentAction"));
    impl_->closeAction->setShortcut(QKeySequence::Close);
    connect(impl_->closeAction, &QAction::triggered, impl_->controller,
            &ImportController::closeDocument);
    fileMenu->addSeparator();
    auto *exit = fileMenu->addAction(QStringLiteral("Выход"));
    exit->setShortcut(QKeySequence::Quit);
    connect(exit, &QAction::triggered, this, &QWidget::close);
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("Вид"));

    auto *toolbar = addToolBar(QStringLiteral("Подготовка"));
    toolbar->setObjectName(QStringLiteral("workflowToolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(open);
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
    addDock(QStringLiteral("projectDock"), QStringLiteral("Проект и гибы"), impl_->project,
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
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Параметры"), scroll,
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
    if (!path.isEmpty()) {
        impl_->controller->open(nativePath(path));
    }
}
ImportController &MainWindow::importController() const {
    return *impl_->controller;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    impl_->controller->closeDocument();
    QMainWindow::closeEvent(event);
}
} // namespace bending::ui
