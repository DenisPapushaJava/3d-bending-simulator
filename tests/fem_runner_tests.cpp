#include "bending/fem/DockerFemRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
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
void waitUntil(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    require(condition(), "Process test timed out");
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "Pass the Docker stub executable");
        QTemporaryDir temp;
        const auto scripts = temp.path() + QStringLiteral("/Скрипты с пробелами");
        QDir().mkpath(scripts);
        for (const auto *name :
             {"gui_job.py", "bend_hole.py", "results.py", "report.py", "viewer.html"}) {
            QFile file(QDir(scripts).filePath(QLatin1String(name)));
            require(file.open(QIODevice::WriteOnly), "Fixture scripts missing");
        }
        const auto state = temp.path() + QStringLiteral("/state");
        qputenv("BENDING_FEM_STUB_STATE", state.toUtf8());
        bending::fem::DockerFemRunner runner(native(scripts),
                                             native(temp.path() + QStringLiteral("/Результаты")),
                                             native(app.arguments()[1]));
        using namespace bending::application;
        std::optional<FemOutcome> outcome;
        const auto start = [&] {
            outcome.reset();
            runner.start(
                {{}, "Контекст D01 · ревизия 4"}, [](std::string) {},
                [&](FemOutcome result) { outcome = std::move(result); });
            require(!outcome, "Completion must be asynchronous");
        };
        for (const auto *scenario : {"valid", "mismatch", "missing", "nonfinite", "exit"}) {
            qputenv("BENDING_FEM_STUB_SCENARIO", scenario);
            start();
            waitUntil([&] { return outcome.has_value(); });
            require(QFile::exists(state + QStringLiteral(".removed")), "Container not removed");
            const bool valid = QByteArray(scenario) == "valid";
            require((outcome->status == FemOutcomeStatus::Completed) == valid,
                    "Invalid protocol result accepted");
            require(outcome->result.has_value() == valid, "Failure published a result");
            if (valid)
                require(outcome->result->unloadedAngleDeg.has_value(), "Unloading result missing");
        }
        qputenv("BENDING_FEM_STUB_SCENARIO", "wait");
        start();
        waitUntil([&] { return QFile::exists(state + QStringLiteral(".started")); });
        runner.cancel();
        runner.cancel();
        waitUntil([&] { return outcome.has_value(); });
        require(outcome->status == FemOutcomeStatus::Cancelled && !outcome->result,
                "Cancelled work published success");
        require(QFile::exists(state + QStringLiteral(".removed")),
                "Cancel did not remove container");
        qputenv("BENDING_FEM_STUB_SCENARIO", "valid");
        start();
        runner.cancel(); // Cancellation during create must not start solving.
        waitUntil([&] { return outcome.has_value(); });
        require(outcome->status == FemOutcomeStatus::Cancelled, "Create cancellation ignored");
        bending::fem::DockerFemRunner missing(native(scripts), native(temp.path()),
                                              native(temp.path() + QStringLiteral("/no-docker")));
        outcome.reset();
        missing.start({}, {}, [&](FemOutcome result) { outcome = std::move(result); });
        waitUntil([&] { return outcome.has_value(); });
        require(outcome->status == FemOutcomeStatus::Failed, "Missing Docker passed");
        qputenv("BENDING_FEM_STUB_SCENARIO", "remove-failed");
        {
            bending::fem::DockerFemRunner failedStop(native(scripts), native(temp.path()),
                                                     native(app.arguments()[1]));
            outcome.reset();
            failedStop.start({}, {}, [&](FemOutcome result) { outcome = std::move(result); });
            waitUntil([&] { return outcome.has_value(); });
            require(outcome->status == FemOutcomeStatus::StopFailed && !outcome->result,
                    "Unconfirmed cleanup reported success");
        }
        std::cout << "FEM process lifecycle and manifest checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
