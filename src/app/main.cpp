#include "bending/Version.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/Kernel.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/GeometryCacheFiles.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/update/UpdateClient.h"
#include "bending/viewer/ViewerWidget.h"
#include <QSettings>

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QPalette>
#include <QProxyStyle>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <algorithm>
#include <filesystem>

namespace {
class ApplicationStyle final : public QProxyStyle {
  public:
    ApplicationStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override {
        // Windows otherwise paints disabled captions twice, with a shifted light copy.
        if (hint == SH_EtchDisabledText)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};
} // namespace

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
    QApplication::setStyle(new ApplicationStyle);
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
    QCoreApplication::setOrganizationName(QStringLiteral("3D Bending Simulator"));
    QCoreApplication::setApplicationName(QStringLiteral("3D Bending Simulator"));
    QGuiApplication::setDesktopFileName(QStringLiteral("3d-bending-simulator"));
    QIcon applicationIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        applicationIcon.addFile(QStringLiteral(":/app/icons/%1.png").arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(applicationIcon);
    QCoreApplication::setApplicationVersion(QString::fromUtf8(
        bending::version.data(), static_cast<qsizetype>(bending::version.size())));
    const auto portableRoot = bending::update::discoverPortableRoot(
        QCoreApplication::applicationDirPath(), QCoreApplication::applicationVersion());
    if (!portableRoot.isEmpty()) {
        const auto file = QDir(portableRoot).filePath(QStringLiteral("data/settings.ini"));
        QDir().mkpath(QFileInfo(file).absolutePath());
        if (!QFileInfo::exists(file)) {
            QSettings legacy;
            QSettings portable(file, QSettings::IniFormat);
            for (const auto &key : legacy.allKeys())
                portable.setValue(key, legacy.value(key));
            portable.sync();
        }
        bending::ui::setPreferencesFile(file);
    }
#ifdef _WIN32
    QSslSocket::setActiveBackend(QStringLiteral("schannel"));
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Импорт и просмотр STEP · симулятор гибки"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption updateProbe(
        QStringLiteral("update-runtime-probe"),
        QStringLiteral("Проверить TLS-комплект без сетевых запросов"));
    const QCommandLineOption updateToken(QStringLiteral("update-token"),
                                         QStringLiteral("Внутренний токен перезапуска"),
                                         QStringLiteral("token"));
    const QCommandLineOption updateRoot(QStringLiteral("portable-root"),
                                        QStringLiteral("Внутренняя папка portable-комплекта"),
                                        QStringLiteral("directory"));
    parser.addOptions({updateProbe, updateToken, updateRoot});
    const QCommandLineOption noToolCache(QStringLiteral("no-tool-cache"),
                                         QStringLiteral("Отключить кэш оснастки для диагностики"));
    const QCommandLineOption toolCacheDirectory(QStringLiteral("tool-cache-dir"),
                                                QStringLiteral("Папка постоянного кэша оснастки"),
                                                QStringLiteral("directory"));
    parser.addOptions({noToolCache, toolCacheDirectory});
    QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                 QStringLiteral("Проверить создание окна и завершиться"));
    parser.addOption(smokeTest);
    QCommandLineOption graphicsTest(
        QStringLiteral("graphics-smoke-test"),
        QStringLiteral("Проверить настоящий OCCT/OpenGL-рендеринг и завершиться"));
    parser.addOption(graphicsTest);
    QCommandLineOption toolCatalogOption(
        QStringLiteral("tool-catalog"),
        QStringLiteral("Дополнить/обновить каталог пресетами из JSON"), QStringLiteral("file"));
    parser.addOption(toolCatalogOption);
    parser.addPositionalArgument(QStringLiteral("step"),
                                 QStringLiteral("STEP одной детали или проект .bend для открытия"),
                                 QStringLiteral("[step]"));
    parser.process(app);
    if (parser.isSet(updateProbe)) {
        const bool ready = QSslSocket::supportsSsl()
#ifdef _WIN32
                           && QSslSocket::activeBackend() == QStringLiteral("schannel")
#endif
            ;
        return ready ? 0 : 1;
    }
    if (parser.positionalArguments().size() > 1) {
        parser.showHelp(1);
    }

    const auto files = std::make_shared<bending::project::ProjectFiles>();
    std::shared_ptr<bending::cad::ToolGeometryCache> toolCache;
    if (!parser.isSet(noToolCache)) {
        const auto cacheDirectory =
            parser.isSet(toolCacheDirectory) ? parser.value(toolCacheDirectory)
            : !portableRoot.isEmpty()
                ? QDir(portableRoot).filePath("data/cache/tool-geometry")
                : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                      .filePath("cache/tool-geometry");
#ifdef _WIN32
        const std::filesystem::path cachePath(cacheDirectory.toStdWString());
#else
        const std::filesystem::path cachePath(cacheDirectory.toStdString());
#endif
        toolCache = std::make_shared<bending::cad::ToolGeometryCache>(
            std::make_shared<bending::project::GeometryCacheFiles>(cachePath));
    }
    const auto previewBuilder = std::make_shared<bending::cad::SingleBendPreviewBuilder>(toolCache);
    bending::core::ToolCatalog tools;
    const auto savedCatalog = (parser.isSet(smokeTest) || parser.isSet(graphicsTest))
                                  ? QString{}
                                  : bending::ui::toolCatalogPath();
    if (parser.isSet(toolCatalogOption) || !savedCatalog.isEmpty()) {
        const auto argument =
            parser.isSet(toolCatalogOption) ? parser.value(toolCatalogOption) : savedCatalog;
#ifdef _WIN32
        const auto path = std::filesystem::path(argument.toStdWString());
#else
        const auto path = std::filesystem::path(argument.toStdString());
#endif
        const auto extra = bending::project::readToolCatalog(path);
        if (const auto *error = std::get_if<std::string>(&extra)) {
            if (parser.isSet(toolCatalogOption)) {
                qCritical() << "Custom tool catalogue failed:" << QString::fromStdString(*error);
                return 1;
            }
            qWarning() << "Saved tool catalogue unavailable:" << QString::fromStdString(*error);
        }
        if (const auto *additional = std::get_if<bending::core::ToolCatalog>(&extra))
            for (const auto &preset : *additional) {
                const auto it = std::find_if(tools.begin(), tools.end(),
                                             [&](const auto &t) { return t.id == preset.id; });
                if (it == tools.end())
                    tools.push_back(preset);
                else
                    *it = preset;
            }
        if (!bending::core::validToolCatalog(tools)) {
            qCritical() << "Merged tool catalogue is invalid";
            return 1;
        }
    }
    bending::update::UpdateClient updates({QCoreApplication::applicationVersion(), {}, 30000},
                                          &app);
    bending::ui::MainWindow window({bending::version, bending::cad::kernelVersion()}, files, files,
                                   previewBuilder, std::move(tools));
    window.setToolCatalogLoader(bending::project::readToolCatalog);
    window.setToolProfileImport(
        std::make_shared<bending::project::DxfToolImporter>(),
        {bending::project::readToolLibrary, bending::project::writeToolLibrary});
    window.setDrawingBuilder(std::make_shared<bending::cad::BendingDrawingBuilder>());
    window.setFlatPatternBuilder(std::make_shared<bending::cad::FlatPatternBuilder>());
    window.setUpdateClient(&updates);
    const bool hasFile = !parser.positionalArguments().isEmpty();
    if (hasFile) {
        window.openStep(parser.positionalArguments().first());
    }
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    if (parser.isSet(graphicsTest) && !hasFile && viewer) {
        viewer->showDiagnosticBox();
    }
    window.show();
    if (parser.isSet(updateToken) &&
        QDir(parser.value(updateRoot)).canonicalPath() == portableRoot && !portableRoot.isEmpty()) {
        auto *acknowledge = new QTimer(&app);
        auto deadline = std::make_shared<QElapsedTimer>();
        deadline->start();
        QObject::connect(acknowledge, &QTimer::timeout, &app, [&, acknowledge, deadline, viewer] {
            if (viewer && viewer->isReady() && window.isVisible()) {
                if (bending::update::acknowledgeUpdate(portableRoot,
                                                       QCoreApplication::applicationVersion(),
                                                       parser.value(updateToken)))
                    window.statusBar()->showMessage(
                        QStringLiteral("Программа обновлена до %1")
                            .arg(QCoreApplication::applicationVersion()),
                        8000);
                acknowledge->stop();
            } else if (deadline->elapsed() > 25000)
                acknowledge->stop();
        });
        acknowledge->start(100);
    }
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
                                   viewer != nullptr &&
                                   !window.windowIcon().pixmap(32, 32).isNull();
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
