#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/update/UpdateClient.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 6000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "Update UI operation timed out");
}
class Writer final : public application::ProjectWriter {
  public:
    mutable std::atomic<bool> fail = false, hold = false;
    project::ProjectFiles files;
    std::optional<std::string>
    save(const std::filesystem::path &path, const application::Document &document,
         std::optional<double> thickness, const std::vector<application::BendPreparation> &bends,
         std::stop_token stop, core::UnfoldSettings unfold,
         const std::optional<application::ProjectDrawingSettings> &drawing,
         const application::ProjectDxf &dxf) const override {
        while (hold && !stop.stop_requested())
            QThread::msleep(1);
        if (fail)
            return "Controlled save failure";
        return files.save(path, document, thickness, bends, stop, unfold, drawing, dxf);
    }
};
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir preferences;
        require(preferences.isValid(), "No private test settings folder");
        ui::setPreferencesFile(preferences.filePath("settings.ini"));
        QTcpServer server;
        require(server.listen(QHostAddress::LocalHost, 0), "No HTTP fixture");
        int requests = 0;
        bool hang = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
                if (!socket->readAll().contains("GET "))
                    return;
                ++requests;
                if (hang)
                    return;
                const QByteArray body =
                    QJsonDocument(
                        QJsonArray{QJsonObject{
                            {"tag_name", "v2026.1.0-alpha.10"},
                            {"draft", false},
                            {"prerelease", true},
                            {"published_at", "2026-10-28T12:00:00Z"},
                            {"body", QStringLiteral("Улучшена читаемость чертежей.\nДобавлены "
                                                    "размеры и развёртка с отверстиями.")},
                            {"assets", QJsonArray{}}}})
                        .toJson();
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        update::UpdateClient client(
            {"2026.1.0-alpha.1", QUrl("http://127.0.0.1:" + QString::number(server.serverPort())),
             3000});
        auto files = std::make_shared<project::ProjectFiles>();
        auto writer = std::make_shared<Writer>();
        ui::MainWindow window({"2026.1.0-alpha.1", "test"}, files, writer);
        window.setAutomaticPartInputPrompt(false);
        window.setUpdateClient(&client);
        window.show();
        QApplication::processEvents();
        require(requests == 0, "Window construction initiated a check");
        auto *action = window.findChild<QAction *>("checkUpdatesAction");
        require(action && action->isEnabled(), "Manual update action missing");
        action->trigger();
        action->trigger();
        auto *dialog = window.findChild<QDialog *>("updateDialog");
        require(dialog && !dialog->isModal() &&
                    window.findChildren<QDialog *>("updateDialog").size() == 1,
                "Update window duplicated or blocked work");
        wait([&] { return requests > 0 && !client.busy(); });
        if (requests != 1 || client.state() != update::UpdateState::Available)
            std::cerr << "Requests: " << requests << "; state: " << static_cast<int>(client.state())
                      << "; message: " << client.message().toStdString() << '\n';
        require(requests == 1 && client.state() == update::UpdateState::Available &&
                    window.isEnabled(),
                "Manual check did not show available release");
        if (const auto snapshot = qEnvironmentVariable("BENDING_UPDATE_SNAPSHOT");
            !snapshot.isEmpty()) {
            QApplication::processEvents(); // Apply layout requests from the network result.
            require(dialog->grab().save(snapshot), "Cannot capture update dialog");
        }
        auto *preview = dialog->findChild<QCheckBox *>("includePrereleases");
        preview->setChecked(false);
        QApplication::processEvents();
        require(requests == 1, "Filter toggle silently requested a network check");
        dialog->findChild<QPushButton *>("updatePrimary")->click();
        wait([&] { return !client.busy(); });
        require(requests == 2 && client.state() == update::UpdateState::Current,
                "Channel selection not respected");
        hang = true;
        action->trigger(); // Raising the existing dialog does not recheck.
        require(requests == 2, "Raising a window initiated a second check");
        client.check(true);
        wait([&] { return requests > 2; });
        dialog->reject();
        QApplication::processEvents();
        require(!client.busy(), "Closing dialog did not cancel check");
        hang = false;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        action->trigger();
        dialog = window.findChild<QDialog *>("updateDialog");
        wait([&] { return !client.busy() && client.state() == update::UpdateState::Available; });
        window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step");
        auto &controller = window.importController();
        wait([&] { return !controller.hasWork(); });
        require(controller.setUnfoldSettings({core::SheetMaterial::MildSteel, {}, 1}),
                "Cannot edit test project");
        int installerCalls = 0;
        bool savedAtLaunch = false;
        QString reopen;
        const auto installer = [&](const QString &path) {
            ++installerCalls;
            reopen = path;
            savedAtLaunch = !controller.hasWork() && !controller.session().modified();
            return QStringLiteral("Controlled launcher failure");
        };
        const auto answer = [&](QMessageBox::StandardButton choice, QString savePath = {},
                                bool cancelFile = false) {
            auto timer = std::make_unique<QTimer>(&window);
            auto answered = std::make_shared<bool>(false);
            QObject::connect(
                timer.get(), &QTimer::timeout, &window,
                [choice, savePath, cancelFile, answered, t = timer.get()] {
                    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                        box && !*answered) {
                        *answered = true;
                        if (auto *button = box->button(choice))
                            button->click();
                    } else if (auto *file =
                                   qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                               file && *answered) {
                        t->stop();
                        if (cancelFile)
                            QMetaObject::invokeMethod(file, "reject", Qt::QueuedConnection);
                        else {
                            file->selectFile(savePath);
                            if (auto *edit = file->findChild<QLineEdit *>("fileNameEdit"))
                                edit->setText(savePath);
                            QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
                        }
                    }
                });
            timer->start(2);
            return timer;
        };
        {
            auto driver = answer(QMessageBox::Cancel);
            window.requestUpdateRestart(installer);
        }
        require(installerCalls == 0 && controller.session().modified() && window.isVisible(),
                "Cancel lost changes or launched updater");
        {
            auto driver = answer(QMessageBox::Discard);
            window.requestUpdateRestart(installer);
        }
        require(installerCalls == 1 && controller.session().modified() && window.isVisible(),
                "Launcher failure closed the app or discarded its live changes");
        {
            auto driver = answer(QMessageBox::Save, {}, true);
            window.requestUpdateRestart(installer);
        }
        require(installerCalls == 1 && controller.session().modified(),
                "Cancelled save proceeded to restart");
        QTemporaryDir temp;
        require(temp.isValid(), "No temp project folder");
        const auto path = temp.filePath("saved.bend");
        {
            auto driver = answer(QMessageBox::Save, path);
            window.requestUpdateRestart(installer);
            wait([&] { return !controller.hasWork(); });
        }
        require(installerCalls == 2 && savedAtLaunch && reopen == path && QFile::exists(path) &&
                    window.isVisible(),
                "Restart did not await successful project save");
        require(controller.setUnfoldSettings({core::SheetMaterial::MildSteel, .4, 1}),
                "Cannot edit saved project");
        writer->fail = true;
        {
            auto driver = answer(QMessageBox::Save);
            window.requestUpdateRestart(installer);
            wait([&] { return !controller.hasWork(); });
        }
        require(installerCalls == 2 && controller.session().modified() &&
                    !controller.saveError().isEmpty(),
                "Save failure permitted restart");
        writer->fail = false;
        writer->hold = true;
        controller.saveProject(std::filesystem::path(path.toStdString()), writer);
        window.requestUpdateRestart(installer);
        dialog->reject(); // Cancel pending restart while the user's save continues normally.
        writer->hold = false;
        wait([&] { return !controller.hasWork(); });
        require(installerCalls == 2 && window.isVisible(),
                "Cancelled pending update restarted after worker completion");
        QFile blocked(preferences.filePath("blocked"));
        require(blocked.open(QIODevice::WriteOnly), "Cannot create settings failure fixture");
        blocked.close();
        ui::setPreferencesFile(blocked.fileName() + "/settings.ini");
        auto dirtySettings = ui::preferences();
        dirtySettings->setValue(QStringLiteral("test/pending"), true);
        window.requestUpdateRestart(installer);
        require(installerCalls == 2 && window.isVisible(),
                "Settings failure allowed the application to exit");
        ui::setPreferencesFile(preferences.filePath("settings.ini"));
        window.requestUpdateRestart([&](const QString &) {
            ++installerCalls;
            return QString{};
        });
        require(installerCalls == 3 && !window.isVisible() && !controller.session().document(),
                "Prepared restart did not close normally");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
