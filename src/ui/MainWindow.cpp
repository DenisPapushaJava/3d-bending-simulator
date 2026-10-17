#include "bending/ui/MainWindow.h"

#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

namespace bending::ui {

MainWindow::MainWindow(RuntimeInfo runtime) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(
        QStringLiteral("3D Bending Simulator · %1")
            .arg(QString::fromUtf8(runtime.applicationVersion.data(),
                                   static_cast<qsizetype>(runtime.applicationVersion.size()))));
    resize(1440, 900);
    auto *viewer = new viewer::ViewerWidget(this);
    setCentralWidget(viewer);

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("Файл"));
    auto *importAction = fileMenu->addAction(QStringLiteral("Импорт STEP…"));
    importAction->setEnabled(false);
    importAction->setToolTip(
        QStringLiteral("Следующий этап — импорт STEP; сейчас показан тестовый примитив"));
    fileMenu->addSeparator();
    auto *exitAction = fileMenu->addAction(QStringLiteral("Выход"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("Вид"));

    auto *toolbar = addToolBar(QStringLiteral("Подготовка"));
    toolbar->setObjectName(QStringLiteral("workflowToolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(importAction);
    toolbar->addSeparator();
    for (const auto *title : {"Деталь", "Инструмент", "Порядок", "Проверка"}) {
        toolbar->addAction(QString::fromUtf8(title))->setEnabled(false);
    }

    auto *camera = addToolBar(QStringLiteral("Камера"));
    camera->setObjectName(QStringLiteral("cameraToolbar"));
    auto *fit = camera->addAction(QStringLiteral("Вписать · F"));
    connect(fit, &QAction::triggered, viewer, &viewer::ViewerWidget::fitAll);
    const auto addOrientation = [&](const QString &title,
                                    viewer::ViewerWidget::Orientation orientation) {
        auto *action = camera->addAction(title);
        connect(action, &QAction::triggered, viewer,
                [viewer, orientation] { viewer->setOrientation(orientation); });
    };
    addOrientation(QStringLiteral("Изометрия"), viewer::ViewerWidget::Orientation::Isometric);
    addOrientation(QStringLiteral("Сверху"), viewer::ViewerWidget::Orientation::Top);
    addOrientation(QStringLiteral("Спереди"), viewer::ViewerWidget::Orientation::Front);
    camera->setEnabled(false);
    connect(
        viewer, &viewer::ViewerWidget::renderingChanged, this, [this, viewer, camera](bool ready) {
            camera->setEnabled(ready);
            statusBar()->showMessage(
                ready ? QStringLiteral("Тестовая сцена · ПКМ: вращение · СКМ: перенос · Колесо: "
                                       "масштаб · ЛКМ: грань")
                      : QStringLiteral("3D-просмотр недоступен: %1").arg(viewer->errorString()));
        });

    auto addDock = [&](const QString &name, const QString &title, QWidget *widget,
                       Qt::DockWidgetArea area) {
        auto *dock = new QDockWidget(title, this);
        dock->setObjectName(name);
        dock->setWidget(widget);
        addDockWidget(area, dock);
        viewMenu->addAction(dock->toggleViewAction());
        return dock;
    };

    auto *bends = new QListWidget(this);
    bends->addItem(QStringLiteral("Тестовый примитив · 100 × 60 × 20 мм"));
    bends->setMinimumWidth(220);
    addDock(QStringLiteral("projectDock"), QStringLiteral("Проект и гибы"), bends,
            Qt::LeftDockWidgetArea);

    const auto propertiesText = QStringLiteral("Тестовый CAD-примитив\n100 × 60 × 20 мм\n\n"
                                               "ЛКМ — выбрать грань.\n\n"
                                               "Импорт и моделирование гибки ещё не реализованы.");
    auto *properties = new QLabel(propertiesText, this);
    connect(viewer, &viewer::ViewerWidget::selectionChanged, properties,
            [properties, propertiesText](const QString &description) {
                properties->setText(
                    description.isEmpty()
                        ? propertiesText
                        : description + QStringLiteral("\n\nВыбрана грань демонстрационного тела.\n"
                                                       "Сгибы ещё не заданы."));
            });
    properties->setWordWrap(true);
    properties->setMargin(16);
    properties->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    properties->setMinimumWidth(260);
    addDock(QStringLiteral("propertiesDock"), QStringLiteral("Параметры"), properties,
            Qt::RightDockWidgetArea);

    auto *sequence =
        new QLabel(QStringLiteral("Ручная последовательность · операции ещё не заданы"), this);
    sequence->setMargin(16);
    sequence->setMinimumHeight(100);
    addDock(QStringLiteral("sequenceDock"), QStringLiteral("Порядок гибов"), sequence,
            Qt::BottomDockWidgetArea);

    statusBar()->showMessage(QStringLiteral("Тестовая сцена · создание OpenGL-контекста…"));
    if (!viewer->errorString().isEmpty()) {
        statusBar()->showMessage(viewer->errorString());
    }
    statusBar()->addPermanentWidget(
        new QLabel(QStringLiteral("Qt %1 · OCCT %2")
                       .arg(QString::fromLatin1(qVersion()),
                            QString::fromLatin1(runtime.cadVersion.data(),
                                                static_cast<qsizetype>(runtime.cadVersion.size()))),
                   this));

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Справка"));
    auto *about = helpMenu->addAction(QStringLiteral("О программе"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("3D Bending Simulator"),
            windowTitle() + QStringLiteral("\n\nПрототип OCCT-просмотрщика на тестовом примитиве.\n"
                                           "Импорт и моделирование гибки ещё не реализованы."));
    });
}

} // namespace bending::ui
