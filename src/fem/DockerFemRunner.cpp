#include "bending/fem/DockerFemRunner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <stdexcept>
#include <utility>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace bending::fem {
namespace {
QString pathText(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}
std::filesystem::path pathValue(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
QString holeName(application::FemHole hole) {
    switch (hole) {
    case application::FemHole::Center:
        return QStringLiteral("center");
    case application::FemHole::Offset6Mm:
        return QStringLiteral("offset6");
    case application::FemHole::None:
        return QStringLiteral("none");
    }
    return {};
}
QString meshName(application::FemMesh mesh) {
    switch (mesh) {
    case application::FemMesh::Coarse:
        return QStringLiteral("coarse");
    case application::FemMesh::Medium:
        return QStringLiteral("medium");
    case application::FemMesh::Fine:
        return QStringLiteral("fine");
    }
    return {};
}
} // namespace
class DockerFemRunner::Impl final : public QObject {
  public:
    Impl(std::filesystem::path scriptsPath, std::filesystem::path resultsPath,
         std::filesystem::path executable)
        : scripts(QFileInfo(pathText(scriptsPath)).absoluteFilePath()),
          results(QFileInfo(pathText(resultsPath)).absoluteFilePath()),
          docker(pathText(executable)) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
            const auto bytes = process.readAllStandardOutput();
            if (log.isOpen()) {
                log.write(bytes);
            }
            tail = (tail + QString::fromUtf8(bytes)).right(4000);
            if (progress) {
                progress(QString::fromUtf8(bytes).toStdString());
            }
        });
        QObject::connect(
            &process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    failure = QStringLiteral("Не удалось запустить Docker. Установите и запустите "
                                             "Docker с Linux-контейнерами.\n%1")
                                  .arg(process.errorString());
                    if (created) {
                        removeContainer();
                    } else {
                        finish();
                    }
                }
            });
        QObject::connect(
            &process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
                if (!completion) {
                    return;
                }
                if (creating) {
                    stageTimer.stop();
                    creating = false;
                    created = exitCode == 0 && status == QProcess::NormalExit;
                    if (!created) {
                        failure = QStringLiteral("Не удалось создать FEM-контейнер. "
                                                 "Проверьте Docker и локальный образ "
                                                 "bending-fem:ubuntu24.04.\n%1")
                                      .arg(tail);
                        finish();
                    } else if (cancelled) {
                        removeContainer();
                    } else {
                        process.start(docker, {QStringLiteral("start"), QStringLiteral("--attach"),
                                               container});
                    }
                } else {
                    if (!cancelled && (exitCode != 0 || status != QProcess::NormalExit)) {
                        failure = QStringLiteral("FEM-расчёт не завершён. Смотрите журнал "
                                                 "и case/solver.log.\n%1")
                                      .arg(tail);
                    }
                    if (!cleaning && !removed) {
                        removeContainer();
                    } else if (removed) {
                        finish();
                    }
                }
            });
        QObject::connect(
            &cleanup, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
                stageTimer.stop();
                cleaning = false;
                removed = true;
                if (code != 0 || status != QProcess::NormalExit) {
                    stopFailed = true;
                    failure =
                        QStringLiteral("Не удалось удалить контейнер %1. "
                                       "Проверьте Docker: docker rm -f %1\n%2")
                            .arg(container, QString::fromUtf8(cleanup.readAllStandardError()));
                }
                if (process.state() != QProcess::NotRunning) {
                    process.kill(); // The container has already been stopped by rm -f.
                } else {
                    finish();
                }
            });
        QObject::connect(&cleanup, &QProcess::errorOccurred, this,
                         [this](QProcess::ProcessError error) {
                             if (error == QProcess::FailedToStart) {
                                 stageTimer.stop();
                                 cleaning = false;
                                 removed = true;
                                 stopFailed = true;
                                 failure = QStringLiteral("Не удалось остановить контейнер %1: %2")
                                               .arg(container, cleanup.errorString());
                                 process.kill();
                                 if (process.state() == QProcess::NotRunning) {
                                     finish();
                                 }
                             }
                         });
        stageTimer.setSingleShot(true);
        QObject::connect(&stageTimer, &QTimer::timeout, this, [this] {
            if (creating) {
                creating = false;
                failure = QStringLiteral("Docker не завершил создание за 30 секунд.");
                process.kill();
                removeContainer();
            } else if (cleaning) {
                stopFailed = true;
                cleanup.kill(); // finished reports unconfirmed cleanup, never successful cancel.
            }
        });
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, this, [this] {
            failure = QStringLiteral("Исчерпан лимит времени FEM (40 минут).");
            cancel();
        });
    }
    ~Impl() override {
        // Normal dialog shutdown waits asynchronously. This is the final application-exit guard.
        completion = {};
        progress = {};
        QObject::disconnect(&process, nullptr, this, nullptr);
        QObject::disconnect(&cleanup, nullptr, this, nullptr);
        if (creating) {
            process.waitForFinished(5000);
        }
        if (!container.isEmpty()) {
            QProcess stop;
            stop.start(docker, {QStringLiteral("rm"), QStringLiteral("-f"), container});
            stop.waitForFinished(5000);
        }
        process.kill();
        process.waitForFinished(3000);
        cleanup.kill();
        cleanup.waitForFinished(3000);
    }
    void start(application::FemRequest request, Progress onProgress, Completion onCompletion) {
        if (completion || stopFailed) {
            QTimer::singleShot(0, this, [done = std::move(onCompletion)] {
                done({application::FemOutcomeStatus::Failed,
                      {},
                      "FEM job already running or external cleanup not confirmed"});
            });
            return;
        }
        progress = std::move(onProgress);
        completion = std::move(onCompletion);
        parameters = request.parameters;
        failure.clear();
        tail.clear();
        cancelled = created = creating = cleaning = removed = stopFailed = false;
        container.clear();
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        directory = QDir(results).filePath(id);
        const auto failSoon = [this](QString detail) {
            failure = std::move(detail);
            QTimer::singleShot(0, this, [this] { finish(); });
        };
        if (holeName(parameters.hole).isEmpty() || meshName(parameters.mesh).isEmpty() ||
            request.context.size() > 4096 || scripts.contains(QLatin1Char(',')) ||
            results.contains(QLatin1Char(','))) {
            failSoon(QStringLiteral("Некорректные параметры или запятая в пути Docker mount."));
            return;
        }
        for (const auto *file :
             {"gui_job.py", "bend_hole.py", "results.py", "report.py", "viewer.html"}) {
            if (!QFileInfo::exists(QDir(scripts).filePath(QLatin1String(file)))) {
                failSoon(QStringLiteral("Не найдены скрипты FEM: %1").arg(scripts));
                return;
            }
        }
        if (!QDir().mkpath(results) || !QDir(results).mkdir(id)) {
            failSoon(QStringLiteral("Не удалось создать каталог результатов: %1").arg(directory));
            return;
        }
        parameterJson = {{QStringLiteral("hole"), holeName(parameters.hole)},
                         {QStringLiteral("mesh"), meshName(parameters.mesh)},
                         {QStringLiteral("unload"), parameters.unload}};
        const QJsonObject input{
            {QStringLiteral("schema"), 1},
            {QStringLiteral("id"), id},
            {QStringLiteral("context"), QString::fromStdString(request.context)},
            {QStringLiteral("parameters"), parameterJson}};
        const auto bytes = QJsonDocument(input).toJson();
        requestHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        QSaveFile file(QDir(directory).filePath(QStringLiteral("request.json")));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            failSoon(QStringLiteral("Не удалось сохранить входные параметры FEM."));
            return;
        }
        log.setFileName(QDir(directory).filePath(QStringLiteral("bridge.log")));
        if (!log.open(QIODevice::WriteOnly)) {
            failSoon(QStringLiteral("Не удалось создать журнал FEM."));
            return;
        }
        container = QStringLiteral("bending-fem-") + id;
        QStringList args{QStringLiteral("create"),
                         QStringLiteral("--pull=never"),
                         QStringLiteral("--init"),
                         QStringLiteral("--network=none"),
                         QStringLiteral("--name"),
                         container,
                         QStringLiteral("--mount"),
                         QStringLiteral("type=bind,src=%1,dst=/opt/bending,readonly").arg(scripts),
                         QStringLiteral("--mount"),
                         QStringLiteral("type=bind,src=%1,dst=/work").arg(directory)};
#ifdef Q_OS_UNIX
        args << QStringLiteral("--user") << QStringLiteral("%1:%2").arg(getuid()).arg(getgid());
#endif
        args << QStringLiteral("--workdir") << QStringLiteral("/opt/bending")
             << QStringLiteral("bending-fem:ubuntu24.04") << QStringLiteral("python3")
             << QStringLiteral("-u") << QStringLiteral("gui_job.py") << QStringLiteral("/work");
        creating = true;
        stageTimer.start(30000);
        timer.start(40 * 60 * 1000);
        process.start(docker, args);
    }
    void cancel() {
        if (!completion || cancelled) {
            return;
        }
        cancelled = true;
        if (created) {
            removeContainer();
        }
        // During create, wait for its completion, then remove without ever starting the solver.
    }
    void removeContainer() {
        if (cleaning || removed) {
            return;
        }
        cleaning = true;
        stageTimer.start(15000);
        cleanup.start(docker, {QStringLiteral("rm"), QStringLiteral("-f"), container});
    }
    application::FemResult readResult() const {
        QFile manifest(QDir(directory).filePath(QStringLiteral("completion.json")));
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 65536) {
            throw std::runtime_error("No complete FEM manifest");
        }
        const auto json = QJsonDocument::fromJson(manifest.readAll()).object();
        if (json.value(QStringLiteral("schema")).toInt() != 1 ||
            json.value(QStringLiteral("id")).toString() != id ||
            json.value(QStringLiteral("status")).toString() !=
                QStringLiteral("completed_synthetic_research") ||
            json.value(QStringLiteral("request_sha256")).toString().toLatin1() != requestHash ||
            json.value(QStringLiteral("parameters")).toObject() != parameterJson) {
            throw std::runtime_error("Mismatched or incomplete FEM result");
        }
        const auto number = [&](const char *name) {
            const auto value = json.value(QLatin1String(name));
            if (!value.isDouble() || !std::isfinite(value.toDouble())) {
                throw std::runtime_error("Missing or non-finite FEM result");
            }
            return value.toDouble();
        };
        const auto animation = QDir(directory).filePath(QStringLiteral("result.html"));
        if (!QFileInfo(animation).isFile() || QFileInfo(animation).size() == 0) {
            throw std::runtime_error("Missing FEM animation");
        }
        application::FemResult result{pathValue(directory),
                                      pathValue(animation),
                                      number("formed_force_N"),
                                      number("formed_angle_deg"),
                                      {}};
        if (parameters.unload) {
            result.unloadedAngleDeg = number("unloaded_angle_deg");
        } else if (!json.value(QStringLiteral("unloaded_angle_deg")).isNull()) {
            throw std::runtime_error("Unexpected FEM unloading result");
        }
        return result;
    }
    void finish() {
        if (!completion) {
            return;
        }
        timer.stop();
        stageTimer.stop();
        log.close();
        application::FemOutcome outcome{
            application::FemOutcomeStatus::Failed, {}, failure.toStdString()};
        if (stopFailed) {
            outcome.status = application::FemOutcomeStatus::StopFailed;
        } else if (failure.isEmpty() && cancelled) {
            outcome.status = application::FemOutcomeStatus::Cancelled;
            outcome.detail = "Расчёт отменён; частичный результат не применяется.";
        } else if (failure.isEmpty()) {
            try {
                outcome.result = readResult();
                outcome.status = application::FemOutcomeStatus::Completed;
            } catch (const std::exception &error) {
                outcome.detail = error.what();
            }
        }
        if (outcome.status == application::FemOutcomeStatus::Failed || stopFailed) {
            outcome.detail += "\nКаталог: " + directory.toStdString();
        }
        if (!stopFailed) {
            container.clear();
        }
        auto done = std::move(completion);
        completion = {};
        progress = {};
        done(std::move(outcome));
    }
    QString scripts, results, docker, id, directory, container, failure, tail;
    QByteArray requestHash;
    QJsonObject parameterJson;
    application::FemParameters parameters;
    QProcess process, cleanup;
    QTimer timer, stageTimer;
    QFile log;
    Progress progress;
    Completion completion;
    bool cancelled = false, creating = false, created = false, cleaning = false, removed = false,
         stopFailed = false;
};
DockerFemRunner::DockerFemRunner(std::filesystem::path scripts, std::filesystem::path results,
                                 std::filesystem::path dockerExecutable)
    : impl_(std::make_unique<Impl>(std::move(scripts), std::move(results),
                                   std::move(dockerExecutable))) {}
DockerFemRunner::~DockerFemRunner() = default;
void DockerFemRunner::start(application::FemRequest request, Progress progress,
                            Completion completion) {
    impl_->start(std::move(request), std::move(progress), std::move(completion));
}
void DockerFemRunner::cancel() {
    impl_->cancel();
}
} // namespace bending::fem
