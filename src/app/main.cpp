#include "bending/Version.h"
#include "bending/cad/Kernel.h"
#include "bending/cad/StepImporter.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPalette>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    const QDir resources(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("../share/opencascade/resources")));
    const auto resource = [&](const char *variable, const char *relative) {
        const auto path = resources.filePath(QString::fromLatin1(relative));
        if (!qEnvironmentVariableIsSet(variable) && QFileInfo::exists(path)) {
            qputenv(variable, QDir::toNativeSeparators(path).toUtf8());
        }
    };
    resource("CSF_STEPDefaults", "XSTEPResource");
    resource("CSF_ShadersDirectory", "Shaders");
    resource("CSF_XSMessage", "XSMessage");
    resource("CSF_SHMessage", "SHMessage");
    resource("CSF_UnitsLexicon", "UnitsAPI/Lexi_Expr.dat");
    resource("CSF_UnitsDefinition", "UnitsAPI/Units.dat");
    QApplication::setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F7F9FB"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Button, QColor("#F0F3F6"));
    palette.setColor(QPalette::WindowText, QColor("#1C2938"));
    palette.setColor(QPalette::Text, QColor("#1C2938"));
    palette.setColor(QPalette::ButtonText, QColor("#1C2938"));
    palette.setColor(QPalette::Highlight, QColor("#087F83"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#83909F"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#83909F"));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#83909F"));
    app.setPalette(palette);
    QCoreApplication::setApplicationName(QStringLiteral("3D Bending Simulator"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(
        bending::version.data(), static_cast<qsizetype>(bending::version.size())));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Импорт и просмотр STEP · симулятор гибки"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                 QStringLiteral("Проверить создание окна и завершиться"));
    parser.addOption(smokeTest);
    QCommandLineOption graphicsTest(
        QStringLiteral("graphics-smoke-test"),
        QStringLiteral("Проверить настоящий OCCT/OpenGL-рендеринг и завершиться"));
    parser.addOption(graphicsTest);
    parser.addPositionalArgument(QStringLiteral("step"),
                                 QStringLiteral("STEP одной детали для открытия"),
                                 QStringLiteral("[step]"));
    parser.process(app);
    if (parser.positionalArguments().size() > 1) {
        parser.showHelp(1);
    }

    bending::ui::MainWindow window({bending::version, bending::cad::kernelVersion()},
                                   std::make_shared<bending::cad::StepImporter>());
    const bool hasFile = !parser.positionalArguments().isEmpty();
    if (hasFile) {
        window.openStep(parser.positionalArguments().first());
    }
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    if (parser.isSet(graphicsTest) && !hasFile && viewer) {
        viewer->showDiagnosticBox();
    }
    window.show();
    if (parser.isSet(graphicsTest) || parser.isSet(smokeTest)) {
        auto *timer = new QTimer(&app);
        QElapsedTimer started;
        started.start();
        QObject::connect(timer, &QTimer::timeout, &app, [&, viewer, timer, started] {
            const auto &session = window.importController().session();
            if (session.error() || started.elapsed() > 30000) {
                timer->stop();
                qCritical() << "STEP/startup check failed:"
                            << (session.error() ? QString::fromStdString(session.error()->detail)
                                                : QStringLiteral("Timeout"));
                app.exit(1);
                return;
            }
            if (hasFile && !session.document()) {
                return;
            }
            if (parser.isSet(smokeTest) && !parser.isSet(graphicsTest)) {
                timer->stop();
                const bool ready = window.isVisible() && window.centralWidget() != nullptr &&
                                   window.findChildren<QDockWidget *>().size() == 3;
                app.exit(ready ? 0 : 1);
            } else if (viewer && viewer->isReady()) {
                timer->stop();
                app.exit(viewer->captureImage().isNull() ? 1 : 0);
            } else if (!viewer || !viewer->errorString().isEmpty()) {
                timer->stop();
                qCritical() << "OCCT graphics check failed:"
                            << (viewer ? viewer->errorString() : QStringLiteral("Viewer missing"));
                app.exit(1);
            }
        });
        timer->start(50);
    }
    return app.exec();
}
