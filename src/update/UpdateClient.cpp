#include "bending/update/UpdateClient.h"
#include "PortablePackage.h"
#include "bending/application/ReleaseVersion.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

namespace bending::update {
namespace {
const QString repository = QStringLiteral("/DenisPapushaJava/3d-bending-simulator");
const QUrl releases(QStringLiteral("https://api.github.com/repos") + repository +
                    QStringLiteral("/releases"));
bool secureUrl(const QUrl &url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.userInfo().isEmpty() &&
           url.port(443) == 443 && url.fragment().isEmpty();
}
bool assetUrl(const QUrl &url, const QString &tag, const QString &name) {
    return secureUrl(url) && url.host() == QStringLiteral("github.com") && url.query().isEmpty() &&
           url.path() == repository + QStringLiteral("/releases/download/") + tag + '/' + name;
}
bool allowedRedirect(const QUrl &url) {
    return secureUrl(url) &&
           (url.host() == QStringLiteral("github.com") ||
            url.host() == QStringLiteral("api.github.com") ||
            url.host() == QStringLiteral("release-assets.githubusercontent.com") ||
            url.host() == QStringLiteral("objects.githubusercontent.com"));
}
QString readText(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() < 1024
               ? QString::fromUtf8(file.readAll()).trimmed()
               : QString{};
}
} // namespace
QString discoverPortableRoot(const QString &directory, const QString &version) {
    QDir bin(directory);
    QDir runtime = bin;
    if (!runtime.cdUp())
        return {};
    QDir root = runtime;
    if (!root.cdUp() || !root.cdUp())
        return {};
    if (runtime.dirName() != version ||
        readText(runtime.filePath(QStringLiteral(".ready"))) != version ||
        readText(root.filePath(QStringLiteral("portable-format.txt"))) != QStringLiteral("1") ||
        readText(root.filePath(QStringLiteral("current.txt"))) != version)
        return {};
    return root.canonicalPath();
}
bool acknowledgeUpdate(const QString &root, const QString &version, const QString &token) {
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(token).hasMatch() ||
        discoverPortableRoot(QCoreApplication::applicationDirPath(), version) !=
            QDir(root).canonicalPath())
        return false;
    const auto pending =
        readText(QDir(root).filePath(QStringLiteral(".updates/pending.txt"))).split('\n');
    if (pending.size() != 3 || pending[1] != version || pending[2] != token)
        return false;
    QSaveFile ack(QDir(root).filePath(QStringLiteral(".updates/ack-") + token));
    return ack.open(QIODevice::WriteOnly) && ack.write("ready\n") == 6 && ack.commit();
}
class UpdateClient::Impl {
  public:
    UpdateClient *owner;
    UpdateOptions options;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> reply;
    UpdateState state = UpdateState::Idle;
    QString message, root, preparedVersion;
    ReleaseInfo release;
    qint64 received = 0, total = 0;
    std::uint64_t generation = 0;
    bool previews = false, sawRelease = false;
    int page = 0;
    QTemporaryDir cache;
    std::unique_ptr<QSaveFile> output;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QString zip;
    std::jthread worker;
    std::atomic<bool> working = false;
    QProcess extraction;
    QString unpacked;
    std::uint64_t extractionGeneration = 0;
    QElapsedTimer progress;
    explicit Impl(UpdateClient *o, UpdateOptions p) : owner(o), options(std::move(p)) {
#ifdef _WIN32
        root =
            discoverPortableRoot(QCoreApplication::applicationDirPath(), options.installedVersion);
#endif
        QObject::connect(
            &extraction, &QProcess::finished, owner, [this](int code, QProcess::ExitStatus status) {
                if (state != UpdateState::Preparing || extractionGeneration != generation) {
                    notify();
                    return;
                }
                if (code != 0 || status != QProcess::NormalExit) {
                    fail(QStringLiteral("Не удалось распаковать пакет. Установка не изменена."));
                    return;
                }
                const auto extracted = unpacked, version = release.version, installation = root;
                runIo(
                    [extracted, version, installation](std::stop_token stop) {
                        const auto error = stageRuntime(extracted, version, installation, stop);
                        QDir(extracted).removeRecursively();
                        return error;
                    },
                    UpdateState::Ready, QStringLiteral("Пакет проверен и готов к перезапуску."));
            });
        QObject::connect(
            &extraction, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError) {
                if (state == UpdateState::Preparing && extractionGeneration == generation)
                    fail(QStringLiteral("Не удалось запустить распаковку пакета."));
            });
    }
    ~Impl() {
        ++generation;
        if (reply)
            reply->abort();
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        if (extraction.state() != QProcess::NotRunning) {
            extraction.kill();
            extraction.waitForFinished(3000);
        }
    }
    void notify() {
        emit owner->changed();
    }
    void fail(const QString &text) {
        state = UpdateState::Error;
        message = text;
        output.reset();
        notify();
    }
    void runIo(std::function<QString(std::stop_token)> work, UpdateState resultState,
               QString text) {
        state = UpdateState::Preparing;
        const auto id = generation;
        working = true;
        worker = std::jthread([this, id, work = std::move(work), resultState,
                               text = std::move(text)](std::stop_token stop) {
            QString error;
            try {
                error = work(stop);
            } catch (const std::exception &) {
                error = QStringLiteral("Не удалось подготовить пакет. Текущая версия сохранена.");
            }
            working = false;
            QMetaObject::invokeMethod(
                owner,
                [this, id, error, resultState, text] {
                    if (id != generation) {
                        notify();
                        return;
                    }
                    if (!error.isEmpty()) {
                        fail(error);
                        return;
                    }
                    state = resultState;
                    if (state == UpdateState::Ready)
                        preparedVersion = release.version;
                    message = text;
                    notify();
                },
                Qt::QueuedConnection);
        });
        notify();
    }
    void fetch(QUrl logical, qint64 limit, bool stream, std::function<void(QByteArray)> done,
               int redirects = 0) {
        const auto id = generation;
        QUrl transport = logical;
        if (!options.testOrigin.isEmpty()) {
            transport = options.testOrigin;
            transport.setPath(options.testOrigin.path() + logical.path());
            transport.setQuery(logical.query());
        }
        QNetworkRequest request(transport);
        request.setRawHeader("User-Agent",
                             "3D-Bending-Simulator/" + options.installedVersion.toUtf8());
        request.setRawHeader("Accept", logical.host() == QStringLiteral("api.github.com")
                                           ? "application/vnd.github+json"
                                           : "application/octet-stream");
        request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(options.timeoutMs);
        auto *current = network.get(request);
        reply = current;
        auto data = std::make_shared<QByteArray>();
        auto timedOut = std::make_shared<bool>(false);
        auto *deadline = new QTimer(current);
        deadline->setSingleShot(true);
        QObject::connect(deadline, &QTimer::timeout, current, [current, timedOut] {
            *timedOut = true;
            current->abort();
        });
        deadline->start(stream ? std::max(options.timeoutMs, 600000) : options.timeoutMs);
        const auto drain = [this, current, id, data, limit, stream] {
            if (id != generation || !current->isOpen())
                return;
            const auto chunk = current->readAll();
            if (current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                return;
            if (stream) {
                received += chunk.size();
                if (received > limit || !output || output->write(chunk) != chunk.size()) {
                    fail(QStringLiteral("Не удалось сохранить пакет или превышен его размер."));
                    current->abort();
                    return;
                }
                hash.addData(chunk);
                if (progress.elapsed() > 75) {
                    progress.restart();
                    notify();
                }
            } else {
                data->append(chunk);
                if (data->size() > limit) {
                    fail(QStringLiteral("Ответ сервера превышает допустимый размер."));
                    current->abort();
                }
            }
        };
        QObject::connect(current, &QIODevice::readyRead, owner, drain);
        QObject::connect(
            current, &QNetworkReply::finished, owner,
            [this, current, id, logical, data, done = std::move(done), stream, limit, redirects,
             timedOut, drain] {
                current->deleteLater();
                if (id != generation || state == UpdateState::Error)
                    return;
                drain();
                if (state == UpdateState::Error)
                    return;
                reply.clear();
                const int status =
                    current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status >= 300 && status < 400) {
                    const auto next = logical.resolved(
                        current->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
                    if (redirects >= 5 || !allowedRedirect(next)) {
                        fail(QStringLiteral(
                            "Сервер перенаправил запрос на неподдерживаемый адрес."));
                        return;
                    }
                    fetch(next, limit, stream, done, redirects + 1);
                    return;
                }
                if (status == 429 ||
                    (status == 403 && current->rawHeader("X-RateLimit-Remaining") == "0")) {
                    fail(QStringLiteral("GitHub временно ограничил проверки. Попробуйте позже."));
                    return;
                }
                if (status != 200 || current->error() != QNetworkReply::NoError) {
                    fail(*timedOut
                             ? QStringLiteral("Сервер не ответил вовремя. Повторите проверку.")
                             : QStringLiteral("Не удалось связаться с GitHub. Проверьте "
                                              "подключение и повторите."));
                    return;
                }
                done(*data);
            });
    }
    void nextPage() {
        if (++page > 20) {
            fail(QStringLiteral("Не удалось просмотреть все выпуски. Откройте страницу релизов."));
            return;
        }
        auto url = releases;
        url.setQuery(QStringLiteral("per_page=100&page=%1").arg(page));
        fetch(url, 5 * 1024 * 1024, false, [this](const QByteArray &bytes) {
            QJsonParseError error;
            const auto json = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !json.isArray()) {
                fail(QStringLiteral("GitHub вернул непонятный ответ. Повторите проверку позже."));
                return;
            }
            const auto installed =
                application::parseReleaseVersion(options.installedVersion.toStdString());
            for (const auto &value : json.array()) {
                const auto item = value.toObject();
                if (!item.value("draft").isBool() || item.value("draft").toBool() ||
                    !item.value("prerelease").isBool() || !item.value("published_at").isString())
                    continue;
                const auto tag = item.value("tag_name").toString();
                const auto version = application::parseReleaseTag(tag.toStdString());
                if (!version)
                    continue;
                if ((version->stage != application::ReleaseStage::Stable) !=
                    item.value("prerelease").toBool())
                    continue;
                sawRelease = true;
                if ((!previews && version->stage != application::ReleaseStage::Stable) ||
                    *version <= *installed)
                    continue;
                const auto prior = application::parseReleaseVersion(release.version.toStdString());
                if (prior && *version <= *prior)
                    continue;
                ReleaseInfo found;
                found.version = QString::fromStdString(application::releaseVersionText(*version));
                found.notes = item.value("body").toString().left(20000);
                found.page = QUrl(QStringLiteral("https://github.com") + repository +
                                  QStringLiteral("/releases/tag/") + tag);
                found.packageName =
                    QString::fromStdString(application::windowsPackageName(*version));
                for (const auto &entry : item.value("assets").toArray()) {
                    const auto asset = entry.toObject();
                    if (asset.value("state").toString() != QStringLiteral("uploaded"))
                        continue;
                    const auto name = asset.value("name").toString();
                    const QUrl address(asset.value("browser_download_url").toString());
                    if (!assetUrl(address, tag, name))
                        continue;
                    if (name == found.packageName && found.package.isEmpty()) {
                        const auto size = asset.value("size").toDouble(-1);
                        if (size > 0 && size <= 1024. * 1024 * 1024 && std::floor(size) == size) {
                            found.package = address;
                            found.size = static_cast<qint64>(size);
                            found.digest = asset.value("digest").toString();
                        }
                    }
                    if (name == QStringLiteral("SHA256SUMS.txt"))
                        found.checksums = address;
                }
                release = found;
            }
            if (json.array().size() == 100) {
                nextPage();
                return;
            }
            state = !release.version.isEmpty() ? UpdateState::Available
                    : sawRelease               ? UpdateState::Current
                                               : UpdateState::NoReleases;
            message = state == UpdateState::Available
                          ? QStringLiteral("Доступна версия %1").arg(release.version)
                      : state == UpdateState::Current
                          ? QStringLiteral("Новых версий выбранного канала не найдено")
                          : QStringLiteral("Опубликованных версий программы пока нет");
            notify();
        });
    }
};
UpdateClient::UpdateClient(UpdateOptions options, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(options))) {}
UpdateClient::~UpdateClient() = default;
UpdateState UpdateClient::state() const {
    return impl_->state;
}
QString UpdateClient::message() const {
    return impl_->message;
}
QString UpdateClient::installedVersion() const {
    return impl_->options.installedVersion;
}
ReleaseInfo UpdateClient::release() const {
    return impl_->release;
}
qint64 UpdateClient::received() const {
    return impl_->received;
}
qint64 UpdateClient::total() const {
    return impl_->total;
}
bool UpdateClient::busy() const {
    return state() == UpdateState::Checking || state() == UpdateState::Downloading ||
           state() == UpdateState::Preparing || impl_->working ||
           impl_->extraction.state() != QProcess::NotRunning;
}
QString UpdateClient::verifiedZip() const {
    return impl_->zip;
}
QString UpdateClient::portableRoot() const {
    return impl_->root;
}
QString UpdateClient::installUnavailableReason() const {
#ifndef _WIN32
    return QStringLiteral(
        "Готовые обновления предназначены для Windows. Для Linux доступен исходный код выпуска.");
#else
    return impl_->root.isEmpty() ? QStringLiteral("Эта сборка запущена вне portable-комплекта. "
                                                  "Скачайте ZIP и распакуйте в новую папку.")
                                 : QString{};
#endif
}
void UpdateClient::check(bool includePrereleases) {
    if (busy())
        return;
    cancel();
    impl_->release = {};
    impl_->zip.clear();
    impl_->preparedVersion.clear();
    impl_->state = UpdateState::Checking;
    impl_->message = QStringLiteral("Проверяем опубликованные версии…");
    impl_->page = 0;
    impl_->previews = includePrereleases;
    impl_->sawRelease = false;
    impl_->notify();
    if (!application::parseReleaseVersion(impl_->options.installedVersion.toStdString())) {
        impl_->fail(QStringLiteral("Не удалось определить установленную версию."));
        return;
    }
    impl_->nextPage();
}
void UpdateClient::cancel() {
    ++impl_->generation;
    if (impl_->reply)
        impl_->reply->abort();
    impl_->reply.clear();
    impl_->output.reset();
    impl_->worker.request_stop();
    impl_->state = impl_->release.version.isEmpty() ? UpdateState::Idle
                   : impl_->zip.isEmpty()           ? UpdateState::Available
                                                    : UpdateState::Downloaded;
    if (impl_->extraction.state() != QProcess::NotRunning)
        impl_->extraction.kill();
    impl_->message = QStringLiteral("Операция отменена. Программа и проекты не изменены.");
    impl_->notify();
}
void UpdateClient::download() {
    if (busy() || impl_->release.package.isEmpty() || impl_->release.checksums.isEmpty())
        return;
    ++impl_->generation;
    impl_->state = UpdateState::Downloading;
    impl_->message = QStringLiteral("Проверяем данные пакета…");
    impl_->received = 0;
    impl_->total = impl_->release.size;
    impl_->zip.clear();
    impl_->notify();
    impl_->fetch(impl_->release.checksums, 1024 * 1024, false, [this](const QByteArray &bytes) {
        QString expected;
        for (const auto &line : QString::fromUtf8(bytes).split('\n')) {
            const auto match =
                QRegularExpression(QStringLiteral("^([0-9a-fA-F]{64})[ \\t]+\\*?([^\\r\\n]+)\\r?$"))
                    .match(line);
            if (match.hasMatch() && match.captured(2).trimmed() == impl_->release.packageName) {
                if (!expected.isEmpty()) {
                    impl_->fail(QStringLiteral("Контрольная сумма пакета неоднозначна."));
                    return;
                }
                expected = match.captured(1).toLower();
            }
        }
        if (expected.isEmpty() || (!impl_->release.digest.isEmpty() &&
                                   impl_->release.digest != QStringLiteral("sha256:") + expected)) {
            impl_->fail(QStringLiteral("Не удалось подтвердить контрольную сумму пакета."));
            return;
        }
        impl_->output =
            std::make_unique<QSaveFile>(impl_->cache.filePath(QStringLiteral("verified.zip")));
        if (!impl_->output->open(QIODevice::WriteOnly)) {
            impl_->fail(QStringLiteral("Нет места или доступа для скачивания пакета."));
            return;
        }
        impl_->hash.reset();
        impl_->progress.start();
        impl_->message = QStringLiteral("Скачиваем версию %1…").arg(impl_->release.version);
        impl_->notify();
        impl_->fetch(impl_->release.package, impl_->release.size, true,
                     [this, expected](const QByteArray &) {
                         if (impl_->received != impl_->release.size ||
                             QString::fromLatin1(impl_->hash.result().toHex()) != expected) {
                             impl_->fail(QStringLiteral(
                                 "Пакет неполный или повреждён. Повторите скачивание."));
                             return;
                         }
                         if (!impl_->output->commit()) {
                             impl_->fail(QStringLiteral("Не удалось сохранить проверенный пакет."));
                             return;
                         }
                         impl_->output.reset();
                         impl_->zip = impl_->cache.filePath(QStringLiteral("verified.zip"));
                         impl_->state = UpdateState::Downloaded;
                         impl_->message = QStringLiteral("Пакет скачан и проверен.");
                         impl_->notify();
                     });
    });
}
void UpdateClient::prepare() {
    if (busy() || verifiedZip().isEmpty() || !installUnavailableReason().isEmpty())
        return;
    QFile script(QStringLiteral(":/update/data/extract-package.ps1"));
    const auto path = impl_->cache.filePath(QStringLiteral("extract.ps1"));
    QFile target(path);
    if (!script.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly) ||
        target.write(script.readAll()) < 1) {
        impl_->fail(QStringLiteral("Не удалось подготовить распаковку."));
        return;
    }
    target.close();
    impl_->unpacked =
        impl_->cache.filePath("unpacked-" + QUuid::createUuid().toString(QUuid::Id128));
    impl_->extractionGeneration = impl_->generation;
    impl_->state = UpdateState::Preparing;
    impl_->message = QStringLiteral("Распаковываем и проверяем новую версию…");
    impl_->notify();
    impl_->extraction.setProgram(
        QDir(qEnvironmentVariable("SystemRoot"))
            .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")));
    impl_->extraction.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                    "-File", path, "-Archive", impl_->zip, "-Destination",
                                    impl_->unpacked, "-Prefix",
                                    impl_->release.packageName.chopped(4)});
    impl_->extraction.start();
    const auto id = impl_->generation;
    QTimer::singleShot(120000, this, [this, id] {
        if (id == impl_->generation && impl_->extraction.state() != QProcess::NotRunning) {
            impl_->extraction.kill();
            impl_->fail(QStringLiteral(
                "Распаковка заняла слишком много времени. Текущая версия сохранена."));
        }
    });
}
void UpdateClient::saveZipTo(const QString &path) {
    if (busy() || verifiedZip().isEmpty() || path.isEmpty())
        return;
    const auto zip = verifiedZip();
    impl_->message = QStringLiteral("Сохраняем ZIP…");
    impl_->runIo([zip, path](std::stop_token stop) { return copyVerifiedZip(zip, path, stop); },
                 UpdateState::Downloaded, QStringLiteral("ZIP сохранён."));
}
QString UpdateClient::startInstaller(const QString &reopenPath) {
    if (state() != UpdateState::Ready || impl_->preparedVersion != impl_->release.version ||
        impl_->root.isEmpty())
        return QStringLiteral("Пакет ещё не готов к установке.");
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    const QDir root(impl_->root);
    if (!root.mkpath(QStringLiteral(".updates")))
        return QStringLiteral("Нет доступа к папке программы.");
    const auto helper =
        root.filePath(QStringLiteral(".updates/helper-") + token + QStringLiteral(".exe"));
    if (!QFile::copy(root.filePath(QStringLiteral("3d-bending-simulator.exe")), helper))
        return QStringLiteral("Не удалось подготовить помощник обновления.");
    QStringList arguments{"--root",     impl_->root,
                          "--activate", impl_->preparedVersion,
                          "--from",     installedVersion(),
                          "--wait-pid", QString::number(QCoreApplication::applicationPid()),
                          "--token",    token};
    if (!reopenPath.isEmpty())
        arguments << "--reopen" << reopenPath;
    if (!QProcess::startDetached(helper, arguments, impl_->root))
        return QStringLiteral("Не удалось запустить помощник. Программа остаётся открытой.");
    return {};
}
} // namespace bending::update
