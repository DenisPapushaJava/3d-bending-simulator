#include "bending/update/UpdateClient.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <iostream>
#include <stdexcept>
using namespace bending::update;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer time;
    time.start();
    while (!condition() && time.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(condition(), "Network operation timed out");
}
QJsonObject release(const QString &version, bool preview = true) {
    const QString name = "3d-bending-simulator-" + version + "-windows-x86_64.zip";
    const QString base =
        "https://github.com/DenisPapushaJava/3d-bending-simulator/releases/download/v" + version +
        '/';
    const QByteArray payload("portable test archive");
    return {
        {"tag_name", "v" + version},
        {"draft", false},
        {"prerelease", preview},
        {"published_at", "2026-10-28T12:00:00Z"},
        {"body", "<img src=x> plain release notes"},
        {"assets",
         QJsonArray{QJsonObject{{"name", name},
                                {"state", "uploaded"},
                                {"size", payload.size()},
                                {"digest", "sha256:" + QString::fromLatin1(
                                                           QCryptographicHash::hash(
                                                               payload, QCryptographicHash::Sha256)
                                                               .toHex())},
                                {"browser_download_url", base + name}},
                    QJsonObject{{"name", "SHA256SUMS.txt"},
                                {"state", "uploaded"},
                                {"size", 200},
                                {"browser_download_url", base + "SHA256SUMS.txt"}}}}};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QFile extractor(QStringLiteral(":/update/data/extract-package.ps1"));
        check(extractor.open(QIODevice::ReadOnly) && extractor.size() > 0,
              "The bundled extraction script is missing");
        if (app.arguments().contains("--probe-url")) {
            const auto base = app.arguments().at(app.arguments().indexOf("--probe-url") + 1);
            UpdateClient client({"2026.1.0-alpha.1", QUrl(base), 500});
            const auto expected = app.arguments().last();
            client.check(true);
            wait([&] { return !client.busy(); });
            if (expected == "error")
                check(client.state() == UpdateState::Error, "Expected controlled network failure");
            else {
                check(client.state() == UpdateState::Available, "No release through simulation");
                client.download();
                wait([&] { return !client.busy(); });
                check(client.state() == UpdateState::Downloaded, "Simulation package not verified");
            }
            std::cout << client.message().toStdString() << '\n';
            return 0;
        }
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "Cannot start local HTTP fixture");
        int calls = 0;
        QString mode = "available";
        const QByteArray payload("portable test archive");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket, buffer] {
                    *buffer += socket->readAll();
                    if (!buffer->contains("\r\n\r\n"))
                        return;
                    ++calls;
                    const auto path = buffer->split(' ').value(1);
                    buffer->clear();
                    if (mode == "hang")
                        return;
                    QByteArray body, headers;
                    int status = 200;
                    if (mode == "rate") {
                        status = 429;
                        body = "{\"message\":\"rate limit\"}";
                    } else if (mode == "html") {
                        status = 502;
                        body = "<html>upstream error</html>";
                    } else if (mode == "invalid")
                        body = "not JSON";
                    else if (path.startsWith("/repos/")) {
                        if (mode == "empty")
                            body = "[]";
                        else if (mode == "page" && !path.contains("page=2")) {
                            QJsonArray many;
                            for (int i = 0; i < 100; ++i)
                                many.append(release("2026.1.0-alpha.1"));
                            body = QJsonDocument(many).toJson();
                        } else {
                            auto draft = release("2029.1.0");
                            draft["draft"] = true;
                            auto sdk = release("2029.1.0");
                            sdk["tag_name"] = "sdk-2030";
                            auto bad = release("2030.1.0");
                            bad["tag_name"] = "v2030.1.0-evil";
                            auto candidate = release("2026.1.0-alpha.10");
                            if (mode == "bad-url") {
                                auto assets = candidate["assets"].toArray();
                                auto asset = assets[0].toObject();
                                asset["browser_download_url"] = "https://evil.invalid/package.zip";
                                assets[0] = asset;
                                candidate["assets"] = assets;
                            }
                            body = QJsonDocument(QJsonArray{release("2026.1.0-alpha.2"), candidate,
                                                            draft, sdk, bad})
                                       .toJson();
                        }
                    } else if (path.endsWith("SHA256SUMS.txt")) {
                        auto sha =
                            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
                        if (mode == "bad-sha")
                            sha.fill('0');
                        body =
                            sha + "  3d-bending-simulator-2026.1.0-alpha.10-windows-x86_64.zip\n";
                    } else if (mode == "redirect") {
                        status = 302;
                        headers = "Location: https://evil.invalid/package.zip\r\n";
                    } else if (mode == "slow") {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                      QByteArray::number(payload.size()) + "\r\n\r\n" +
                                      payload.left(3));
                        return;
                    } else
                        body = mode == "truncated" ? payload.chopped(1) : payload;
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " result\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n" + headers + "\r\n" +
                                  body);
                    socket->disconnectFromHost();
                });
            }
        });
        UpdateClient client({"2026.1.0-alpha.1",
                             QUrl("http://127.0.0.1:" + QString::number(server.serverPort())),
                             150});
        QElapsedTimer idle;
        idle.start();
        while (idle.elapsed() < 250) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        check(calls == 0 && client.state() == UpdateState::Idle,
              "Update check ran without a manual command");
        client.check(true);
        client.check(true);
        wait([&] { return !client.busy(); });
        check(calls == 1 && client.release().version == "2026.1.0-alpha.10",
              "Duplicate check or incorrect numeric release selection");
        client.download();
        wait([&] { return !client.busy(); });
        check(client.state() == UpdateState::Downloaded, "Verified download failed");
        QFile file(client.verifiedZip());
        check(file.open(QIODevice::ReadOnly) && file.readAll() == payload,
              "Downloaded bytes changed");
        file.close();
        client.check(false);
        wait([&] { return !client.busy(); });
        check(client.state() == UpdateState::Current, "Stable channel selected an alpha");
        mode = "page";
        const auto previous = calls;
        client.check(true);
        wait([&] { return !client.busy(); });
        check(calls == previous + 2 && client.state() == UpdateState::Available,
              "Release pagination was incomplete");
        for (const auto &failure : {QStringLiteral("rate"), QStringLiteral("html"),
                                    QStringLiteral("invalid"), QStringLiteral("hang")}) {
            mode = failure;
            client.check(true);
            wait([&] { return !client.busy(); });
            check(client.state() == UpdateState::Error,
                  "Network failure was reported as up-to-date");
        }
        mode = "empty";
        client.check(true);
        wait([&] { return !client.busy(); });
        check(client.state() == UpdateState::NoReleases, "Empty releases mishandled");
        mode = "hang";
        client.check(true);
        client.cancel();
        mode = "available";
        client.check(true);
        wait([&] { return !client.busy(); });
        check(client.state() == UpdateState::Available, "Cancelled check poisoned its replacement");
        for (const auto &failure :
             {QStringLiteral("bad-sha"), QStringLiteral("truncated"), QStringLiteral("redirect")}) {
            mode = "available";
            client.check(true);
            wait([&] { return !client.busy(); });
            mode = failure;
            client.download();
            wait([&] { return !client.busy(); });
            check(client.state() == UpdateState::Error && client.verifiedZip().isEmpty(),
                  "Unsafe or incomplete package accepted");
        }
        mode = "bad-url";
        client.check(true);
        wait([&] { return !client.busy(); });
        check(client.release().package.isEmpty(), "Foreign asset URL accepted");
        mode = "available";
        client.check(true);
        wait([&] { return !client.busy(); });
        mode = "slow";
        client.download();
        wait([&] { return client.received() > 0; });
        client.cancel();
        check(!client.busy() && client.verifiedZip().isEmpty(),
              "Cancelled partial ZIP was considered ready");
        mode = "available";
        client.download();
        wait([&] { return !client.busy(); });
        check(client.state() == UpdateState::Downloaded,
              "Cancelled download prevented a fresh verified download");
        const auto closedPort = server.serverPort();
        server.close();
        UpdateClient offline(
            {"2026.1.0-alpha.1", QUrl("http://127.0.0.1:" + QString::number(closedPort)), 150});
        offline.check(true);
        wait([&] { return !offline.busy(); });
        check(offline.state() == UpdateState::Error,
              "Connection refusal was reported as up-to-date");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
