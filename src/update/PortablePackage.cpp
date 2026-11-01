#include "PortablePackage.h"
#include "bending/application/ReleaseVersion.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <cmath>

namespace bending::update {
bool safePackagePath(const QString &path) {
    if (path.isEmpty() || path.size() > 240 || path.startsWith('/') || path.contains('\\') ||
        path.contains(':'))
        return false;
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || reserved.match(part).hasMatch())
            return false;
        for (const auto c : part)
            if (c.unicode() < 32 || QStringLiteral("<>\"|?*").contains(c))
                return false;
    }
    return true;
}
namespace {
QByteArray readSmall(const QString &path, qint64 maximum = 8 * 1024 * 1024) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.size() <= maximum ? f.readAll() : QByteArray{};
}
QString copyChecked(const QString &source, const QString &target, qint64 expectedSize,
                    const QByteArray &expectedHash, std::stop_token stop) {
    QFile input(source);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || input.size() != expectedSize ||
        !output.open(QIODevice::WriteOnly))
        return QStringLiteral("Не удалось прочитать пакет или записать новую версию. Проверьте "
                              "место и права доступа.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!input.atEnd()) {
        if (stop.stop_requested())
            return QStringLiteral("Подготовка отменена.");
        const auto chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFile::NoError)
            return QStringLiteral("Ошибка чтения пакета.");
        count += chunk.size();
        if (count > expectedSize || output.write(chunk) != chunk.size())
            return QStringLiteral("Не удалось записать пакет. Проверьте свободное место.");
        hash.addData(chunk);
    }
    if (count != expectedSize || (!expectedHash.isEmpty() && hash.result().toHex() != expectedHash))
        return QStringLiteral("Файлы пакета повреждены. Скачайте обновление заново.");
    if (stop.stop_requested() || !output.commit())
        return QStringLiteral("Подготовка отменена или файл недоступен для записи.");
    return {};
}
bool regularTree(const QString &root) {
    if (QFileInfo(root).isSymLink())
        return false;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().isSymLink() || (!it.fileInfo().isFile() && !it.fileInfo().isDir()))
            return false;
    }
    return true;
}
bool amd64Executable(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto header = file.read(64);
    if (header.size() != 64 || header.left(2) != "MZ")
        return false;
    const auto offset =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(header.constData() + 60));
    if (offset > 1024 * 1024 || !file.seek(offset))
        return false;
    const auto pe = file.read(6);
    return pe.size() == 6 && pe.left(4) == QByteArray("PE\0\0", 4) &&
           qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(pe.constData() + 4)) ==
               0x8664;
}
} // namespace
QString copyVerifiedZip(const QString &source, const QString &destination, std::stop_token stop) {
    if (QFileInfo(source).canonicalFilePath() == QFileInfo(destination).canonicalFilePath())
        return {};
    return copyChecked(source, destination, QFileInfo(source).size(), {}, stop);
}
QString stageRuntime(const QString &extracted, const QString &version, const QString &root,
                     std::stop_token stop) {
    if (!application::parseReleaseVersion(version.toStdString()) || root.isEmpty() ||
        readSmall(QDir(root).filePath("portable-format.txt"), 64).trimmed() != "1")
        return QStringLiteral("Некорректная версия или папка программы.");
    const QDir unpacked(extracted);
    if (readSmall(unpacked.filePath("portable-format.txt"), 64).trimmed() != "1" ||
        readSmall(unpacked.filePath("current.txt"), 128).trimmed() != version.toUtf8() ||
        !regularTree(extracted))
        return QStringLiteral("Пакет не поддерживает безопасное portable-обновление. Скачайте ZIP "
                              "и распакуйте в новую папку.");
    const QSet<QString> rootNames{"3d-bending-simulator.exe", "portable-format.txt", "current.txt",
                                  "README.txt", "runtime"};
    for (const auto &name :
         unpacked.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
        if (!rootNames.contains(name))
            return QStringLiteral("Пакет содержит неподдерживаемые файлы.");
    const QDir source(unpacked.filePath("runtime/" + version));
    if (QDir(unpacked.filePath("runtime")).entryList(QDir::Dirs | QDir::NoDotAndDotDot) !=
        QStringList{version})
        return QStringLiteral("Структура версий в пакете некорректна.");
    const auto bytes = readSmall(source.filePath("runtime-manifest.json"));
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(bytes, &parse).object();
    if (parse.error != QJsonParseError::NoError || json.value("format").toInt() != 1 ||
        json.value("application").toString() != "3d-bending-simulator" ||
        json.value("version").toString() != version ||
        json.value("platform").toString() != "windows-x86_64" ||
        json.value("bootstrapProtocol").toInt() != 1)
        return QStringLiteral("Пакет другой версии, архитектуры или формата обновления.");
    const auto files = json.value("files").toArray();
    if (files.isEmpty() || files.size() > 20000)
        return QStringLiteral("Некорректный список файлов пакета.");
    QSet<QString> names, folded;
    qint64 total = 0;
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    for (const auto &value : files) {
        const auto item = value.toObject();
        const auto path = item.value("path").toString();
        const double size = item.value("size").toDouble(-1);
        if (!safePackagePath(path) ||
            !(path.startsWith("bin/") || path.startsWith("share/") || path == "VERSION") ||
            folded.contains(path.toCaseFolded()) || size < 0 || size > 1024. * 1024 * 1024 ||
            std::floor(size) != size || !sha.match(item.value("sha256").toString()).hasMatch())
            return QStringLiteral("Некорректный или небезопасный путь в пакете.");
        total += static_cast<qint64>(size);
        if (total > 2LL * 1024 * 1024 * 1024)
            return QStringLiteral("Распакованный пакет слишком велик.");
        names.insert(path);
        folded.insert(path.toCaseFolded());
    }
    if (!names.contains("bin/3d-bending-simulator.exe") || !names.contains("VERSION") ||
        readSmall(source.filePath("VERSION"), 128).trimmed() != version.toUtf8() ||
        !amd64Executable(source.filePath("bin/3d-bending-simulator.exe")))
        return QStringLiteral("В пакете нет подходящей Windows x64 программы.");
    QSet<QString> actual;
    QDirIterator disk(source.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (disk.hasNext())
        actual.insert(source.relativeFilePath(disk.next()));
    if (actual != (names | QSet<QString>{"runtime-manifest.json", ".ready"}))
        return QStringLiteral("Состав пакета не совпадает с его описанием.");
    QDir installation(root);
    for (const auto &folder : {QStringLiteral("runtime"), QStringLiteral(".updates")}) {
        const auto path = installation.filePath(folder);
        if (QFileInfo(path).isSymLink() || !installation.mkpath(folder))
            return QStringLiteral("Нет доступа к папке программы или она является ссылкой.");
    }
    const auto destination = installation.filePath("runtime/" + version);
    if (QFileInfo::exists(destination)) {
        // Never overwrite a version folder: it may be running or contain user files.
        if (readSmall(QDir(destination).filePath("runtime-manifest.json")) == bytes &&
            readSmall(QDir(destination).filePath(".ready"), 128).trimmed() == version.toUtf8() &&
            regularTree(destination)) {
            bool matches = true;
            for (const auto &entry : files) {
                if (stop.stop_requested())
                    return QStringLiteral("Подготовка отменена.");
                const auto item = entry.toObject();
                QFile file(QDir(destination).filePath(item.value("path").toString()));
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!file.open(QIODevice::ReadOnly) ||
                    file.size() != static_cast<qint64>(item.value("size").toDouble()) ||
                    !hash.addData(&file) ||
                    hash.result().toHex() != item.value("sha256").toString().toLatin1()) {
                    matches = false;
                    break;
                }
            }
            if (matches)
                return {};
        }
        return QStringLiteral("Папка этой версии уже существует. Запустите программу через "
                              "основной ярлык или распакуйте ZIP отдельно.");
    }
    const auto incoming =
        installation.filePath(".updates/incoming-" + QUuid::createUuid().toString(QUuid::Id128));
    QDir().mkpath(incoming);
    const auto cleanup = [&] { QDir(incoming).removeRecursively(); };
    for (const auto &value : files) {
        const auto item = value.toObject();
        const auto path = item.value("path").toString();
        const auto target = QDir(incoming).filePath(path);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            cleanup();
            return QStringLiteral("Не удалось создать папки новой версии.");
        }
        const auto error = copyChecked(source.filePath(path), target,
                                       static_cast<qint64>(item.value("size").toDouble()),
                                       item.value("sha256").toString().toLatin1(), stop);
        if (!error.isEmpty()) {
            cleanup();
            return error;
        }
    }
    if (!QFile::copy(source.filePath("runtime-manifest.json"),
                     QDir(incoming).filePath("runtime-manifest.json"))) {
        cleanup();
        return QStringLiteral("Не удалось сохранить описание новой версии.");
    }
    QSaveFile ready(QDir(incoming).filePath(".ready"));
    if (!ready.open(QIODevice::WriteOnly) ||
        ready.write(version.toUtf8()) != version.toUtf8().size() || !ready.commit() ||
        stop.stop_requested()) {
        cleanup();
        return QStringLiteral("Подготовка новой версии отменена.");
    }
    if (!QDir().rename(incoming, destination)) {
        cleanup();
        return QStringLiteral("Не удалось завершить подготовку новой версии.");
    }
    return {};
}
} // namespace bending::update
