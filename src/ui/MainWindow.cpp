#include "bending/ui/MainWindow.h"

#include "bending/viewer/ViewerPlaceholder.h"

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
    setCentralWidget(new viewer::ViewerPlaceholder(this));

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("Файл"));
    auto *importAction = fileMenu->addAction(QStringLiteral("Импорт STEP…"));
    importAction->setEnabled(false);
    importAction->setToolTip(QStringLiteral("Будет доступно после подключения CAD-просмотрщика"));
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
    bends->addItem(QStringLiteral("Деталь не загружена"));
    bends->setMinimumWidth(220);
    addDock(QStringLiteral("projectDock"), QStringLiteral("Проект и гибы"), bends,
            Qt::LeftDockWidgetArea);

    auto *properties = new QLabel(QStringLiteral("Выберите деталь или операцию.\n\n"
                                                 "Геометрическая проверка пока не реализована."),
                                  this);
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

    statusBar()->showMessage(QStringLiteral("Локальный проект · мм · градусы"));
    statusBar()->addPermanentWidget(
        new QLabel(QStringLiteral("Qt %1 · OCCT %2")
                       .arg(QString::fromLatin1(qVersion()),
                            QString::fromLatin1(runtime.cadVersion.data(),
                                                static_cast<qsizetype>(runtime.cadVersion.size()))),
                   this));

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Справка"));
    auto *about = helpMenu->addAction(QStringLiteral("О программе"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("3D Bending Simulator"),
                           windowTitle() +
                               QStringLiteral("\n\nНачальный каркас приложения.\n"
                                              "Импорт, 3D-просмотр и гибка ещё не реализованы."));
    });
}

} // namespace bending::ui
