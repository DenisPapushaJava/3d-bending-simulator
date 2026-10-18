// Fake Docker CLI for process lifecycle/protocol tests; never runs a solver.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto state = qEnvironmentVariable("BENDING_FEM_STUB_STATE");
    const auto scenario = qEnvironmentVariable("BENDING_FEM_STUB_SCENARIO");
    if (args.size() < 2 || state.isEmpty()) {
        return 2;
    }
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (args[1] == QLatin1String("create")) {
        QString directory;
        for (const auto &arg : args) {
            if (arg.startsWith(QLatin1String("type=bind,src=")) &&
                arg.endsWith(QLatin1String(",dst=/work"))) {
                directory = arg.mid(14, arg.size() - 14 - 10);
            }
        }
        if (directory.isEmpty() || !write(state, directory.toUtf8())) {
            return 3;
        }
        QFile::remove(state + QStringLiteral(".removed"));
        QTextStream(stdout) << "stub-container\n";
        return 0;
    }
    if (args[1] == QLatin1String("rm")) {
        if (scenario == QLatin1String("remove-failed")) {
            return 9;
        }
        return write(state + QStringLiteral(".removed"), "removed") ? 0 : 4;
    }
    if (args[1] != QLatin1String("start")) {
        return 5;
    }
    QFile location(state);
    if (!location.open(QIODevice::ReadOnly)) {
        return 6;
    }
    const QDir directory(QString::fromUtf8(location.readAll()));
    if (scenario == QLatin1String("wait")) {
        write(state + QStringLiteral(".started"), "running");
        for (int i = 0; i < 500 && !QFile::exists(state + QStringLiteral(".removed")); ++i) {
            QThread::msleep(10);
        }
        return 137;
    }
    if (scenario == QLatin1String("exit")) {
        return 7;
    }
    if (scenario == QLatin1String("missing")) {
        return 0;
    }
    QFile request(directory.filePath(QStringLiteral("request.json")));
    if (!request.open(QIODevice::ReadOnly)) {
        return 8;
    }
    const auto bytes = request.readAll();
    const auto input = QJsonDocument::fromJson(bytes).object();
    QJsonObject result{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("id"), input.value(QStringLiteral("id"))},
        {QStringLiteral("request_sha256"),
         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("parameters"), input.value(QStringLiteral("parameters"))},
        {QStringLiteral("status"), QStringLiteral("completed_synthetic_research")},
        {QStringLiteral("formed_force_N"), 275.0},
        {QStringLiteral("formed_angle_deg"), 30.8},
        {QStringLiteral("unloaded_angle_deg"), input.value(QStringLiteral("parameters"))
                                                       .toObject()
                                                       .value(QStringLiteral("unload"))
                                                       .toBool()
                                                   ? QJsonValue(27.2)
                                                   : QJsonValue(QJsonValue::Null)}};
    if (scenario == QLatin1String("mismatch")) {
        result[QStringLiteral("id")] = QStringLiteral("old-job");
    }
    if (scenario == QLatin1String("nonfinite")) {
        result[QStringLiteral("formed_force_N")] = QJsonValue(QJsonValue::Null);
    }
    write(directory.filePath(QStringLiteral("completion.json")), QJsonDocument(result).toJson());
    write(directory.filePath(QStringLiteral("result.html")), "<!doctype html><title>stub</title>");
    return 0;
}
