#include "bending/fem/DockerFemRunner.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/FemDialog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <functional>
#include <iostream>
#include <stdexcept>

class UrlCapture final : public QObject {
    Q_OBJECT
  public:
    QUrl url;
  public slots:
    void capture(const QUrl &value) {
        url = value;
    }
};
namespace {
using namespace bending::application;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
void waitUntil(const std::function<bool()> &condition, int budget = 10000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < budget) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    require(condition(), "FEM UI wait timed out");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}
class FakeRunner final : public FemRunner {
  public:
    FemRequest request;
    Completion done;
    int starts = 0, cancels = 0;
    void start(FemRequest input, Progress, Completion completion) override {
        request = std::move(input);
        done = std::move(completion);
        ++starts;
    }
    void cancel() override {
        ++cancels;
    }
};
void testDialog() {
    auto runner = std::make_shared<FakeRunner>();
    bending::ui::FemDialog dialog(runner);
    dialog.setCadContext(QStringLiteral("Деталь.step · D01 · ревизия 1"));
    dialog.show();
    auto *start = dialog.findChild<QPushButton *>(QStringLiteral("femStart"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("femCancel"));
    auto *open = dialog.findChild<QPushButton *>(QStringLiteral("femOpenAnimation"));
    auto *mesh = dialog.findChild<QComboBox *>(QStringLiteral("femMesh"));
    auto *result = dialog.findChild<QLabel *>(QStringLiteral("femResult"));
    require(start && cancel && open && mesh && result, "FEM widgets missing");
    start->click();
    start->click();
    require(runner->starts == 1 && dialog.isRunning() && !mesh->isEnabled(),
            "Concurrent FEM launch allowed");
    require(runner->request.parameters == FemParameters{} &&
                runner->request.context.find("D01") != std::string::npos,
            "FEM input/context not captured");
    dialog.setCadContext(QStringLiteral("Другая деталь · ревизия 2"));
    auto old = runner->done;
    QTemporaryDir dir;
    const auto animation = native(dir.path() + QStringLiteral("/result.html"));
    const FemOutcome success{
        FemOutcomeStatus::Completed, FemResult{native(dir.path()), animation, 275, 30.8, 27.2}, {}};
    runner->done(success);
    require(!dialog.isRunning() && open->isEnabled() &&
                result->text().contains(QLatin1String("275")),
            "Finished FEM not displayed");
    require(dialog.findChild<QLabel *>(QStringLiteral("femJobContext"))
                ->text()
                .contains(QStringLiteral("CAD-контекст изменился")),
            "Changed CAD context not identified");
    UrlCapture capture;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &capture, "capture");
    open->click();
    require(capture.url.toLocalFile() == QString::fromStdString(animation.string()),
            "Wrong animation opened");
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    mesh->setCurrentIndex(2);
    require(!open->isEnabled(), "Edited inputs retain an active old result");
    start->click();
    old(success);
    require(dialog.isRunning() && !open->isEnabled(), "Late old completion replaced current job");
    cancel->click();
    runner->done(success);
    require(!open->isEnabled() && !dialog.isRunning() && runner->cancels == 1,
            "Cancelled completion accepted");
    start->click();
    dialog.close();
    require(dialog.isRunning() && runner->cancels == 2, "Close did not cancel work");
    runner->done({FemOutcomeStatus::Cancelled, {}, {}});
    require(!dialog.isVisible() && !dialog.isRunning(),
            "Dialog closed before process stop or failed to close");
    dialog.show();
    start->click();
    dialog.close();
    runner->done({FemOutcomeStatus::StopFailed, {}, "docker rm -f test-container"});
    require(dialog.isVisible() && !start->isEnabled() && !open->isEnabled(),
            "Unconfirmed stop was hidden or allowed another run");
    dialog.close();
}
void testEntry(std::shared_ptr<FemRunner> runner, bool live, const QString &output) {
    auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", "test"}, files, files, {}, runner);
    window.show();
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                    QStringLiteral("/bend-zone-hole-6mm.step"));
    waitUntil([&] { return !window.importController().hasWork(); });
    require(window.importController().session().document() != nullptr,
            "Diagnostic example failed to import");
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    require(list && list->model()->rowCount() == 1, "Diagnostic entry missing");
    list->setCurrentIndex(list->model()->index(0, 0));
    auto *entry = window.findChild<QPushButton *>(QStringLiteral("researchOpening"));
    require(entry && !entry->isHidden() && entry->isEnabled(), "D entry has no FEM command");
    entry->click();
    auto *dialog = window.findChild<bending::ui::FemDialog *>();
    require(dialog && dialog->isVisible(), "FEM dialog did not open from D");
    require(dialog->findChild<QLabel *>(QStringLiteral("femContext"))
                ->text()
                .contains(QLatin1String("D01")),
            "D context missing");
    const auto document = window.importController().session().document();
    const auto edit = window.importController().session().editRevision();
    if (live) {
        QDir().mkpath(output);
        dialog->grab().save(QDir(output).filePath(QStringLiteral("fem-start.png")));
        dialog->findChild<QPushButton *>(QStringLiteral("femStart"))->click();
        require(dialog->isRunning(), "FEM did not start");
        const bool cancelLive = qEnvironmentVariableIsSet("BENDING_FEM_CANCEL_LIVE");
        if (cancelLive) {
            QElapsedTimer running;
            running.start();
            waitUntil([&] { return running.elapsed() > 3000; });
            dialog->findChild<QPushButton *>(QStringLiteral("femCancel"))->click();
        }
        waitUntil([&] { return !dialog->isRunning(); }, 2400000);
        if (cancelLive) {
            require(
                !dialog->findChild<QPushButton *>(QStringLiteral("femOpenAnimation"))->isEnabled(),
                "Cancelled real FEM published a result");
            require(dialog->findChild<QLabel *>(QStringLiteral("femStatus"))
                        ->text()
                        .contains(QStringLiteral("отменён")),
                    "Real FEM cancellation failed");
            dialog->grab().save(QDir(output).filePath(QStringLiteral("fem-cancelled.png")));
            dialog->close();
            window.close();
            return;
        }
        auto *open = dialog->findChild<QPushButton *>(QStringLiteral("femOpenAnimation"));
        require(open->isEnabled(),
                qPrintable(dialog->findChild<QLabel *>(QStringLiteral("femStatus"))->text()));
        require(window.importController().session().document() == document &&
                    window.importController().session().editRevision() == edit,
                "FEM changed the CAD document or project revision");
        dialog->grab().save(QDir(output).filePath(QStringLiteral("fem-complete.png")));
        std::cout << dialog->findChild<QLabel *>(QStringLiteral("femResult"))->text().toStdString()
                  << '\n';
    }
    if (!live) {
        auto fake = std::dynamic_pointer_cast<FakeRunner>(runner);
        dialog->findChild<QPushButton *>(QStringLiteral("femStart"))->click();
        window.close();
        require(window.isVisible() && fake->cancels == 1, "Main window did not wait for FEM stop");
        fake->done({FemOutcomeStatus::Cancelled, {}, {}});
        waitUntil([&] { return !window.isVisible(); });
    }
    dialog->close();
    window.close();
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        if (app.arguments().contains(QStringLiteral("--review"))) {
            require(argc == 4, "--review completed-job screenshot");
            const QDir directory(app.arguments()[2]);
            const auto read = [&](const char *name) {
                QFile file(directory.filePath(QLatin1String(name)));
                require(file.open(QIODevice::ReadOnly), "Cannot open recorded job");
                return QJsonDocument::fromJson(file.readAll()).object();
            };
            const auto request = read("request.json"), completion = read("completion.json");
            auto runner = std::make_shared<FakeRunner>();
            bending::ui::FemDialog dialog(runner);
            dialog.setCadContext(request.value(QStringLiteral("context")).toString());
            dialog.show();
            dialog.findChild<QPushButton *>(QStringLiteral("femStart"))->click();
            runner->done(
                {FemOutcomeStatus::Completed,
                 FemResult{native(directory.path()),
                           native(directory.filePath(QStringLiteral("result.html"))),
                           completion.value(QStringLiteral("formed_force_N")).toDouble(),
                           completion.value(QStringLiteral("formed_angle_deg")).toDouble(),
                           completion.value(QStringLiteral("unloaded_angle_deg")).toDouble()},
                 {}});
            waitUntil([&] { return !dialog.isRunning(); });
            require(dialog.grab().save(app.arguments()[3]), "Cannot save UI capture");
            return 0;
        }
        const bool live = app.arguments().contains(QStringLiteral("--live"));
        if (live) {
            require(argc == 4, "--live scripts output");
            auto runner = std::make_shared<bending::fem::DockerFemRunner>(
                native(app.arguments()[2]), native(app.arguments()[3] + QStringLiteral("/jobs")));
            testEntry(runner, true, app.arguments()[3]);
        } else {
            testDialog();
            testEntry(std::make_shared<FakeRunner>(), false, {});
        }
        std::cout << "FEM UI checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#include "fem_ui_tests.moc"
