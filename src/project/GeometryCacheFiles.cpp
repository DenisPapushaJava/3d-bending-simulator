#include "bending/project/GeometryCacheFiles.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>

namespace bending::project {
namespace {
constexpr qint64 maximumBytes = 64 * 1024 * 1024;
QByteArray view(std::string_view s) {
    return QByteArray::fromRawData(s.data(), static_cast<qsizetype>(s.size()));
}
QString directoryName(const std::filesystem::path &p) {
#ifdef _WIN32
    return QString::fromStdWString(p.native());
#else
    return QString::fromStdString(p.native());
#endif
}
QByteArray digest(std::string_view bytes) {
    return QCryptographicHash::hash(view(bytes), QCryptographicHash::Sha256).toHex();
}
QString fileName(const std::filesystem::path &directory, std::string_view descriptor) {
    return QDir(directoryName(directory))
        .filePath(QString::fromLatin1(digest(descriptor)) + ".brepc");
}
} // namespace
GeometryCacheFiles::GeometryCacheFiles(std::filesystem::path directory)
    : directory_(std::move(directory)) {}
bool GeometryCacheFiles::cannotWrite() const {
    if (!warned_.test_and_set())
        qWarning("Tool geometry cache: cannot save artifacts in %s; using memory only",
                 directoryName(directory_).toUtf8().constData());
    return false;
}
std::optional<std::string> GeometryCacheFiles::read(std::string_view descriptor) const {
    if (directory_.empty())
        return {};
    QFile file(fileName(directory_, descriptor));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 147 || file.size() > maximumBytes + 147)
        return {};
    const auto header = file.read(147);
    if (header != "BEND-GEOMETRY-1\n" + digest(descriptor) + '\n' + header.mid(81, 64) + "\n\n")
        return {};
    const auto payload = file.readAll();
    if (file.error() != QFileDevice::NoError || payload.isEmpty() ||
        digest(std::string_view(payload.constData(), static_cast<std::size_t>(payload.size()))) !=
            header.mid(81, 64))
        return {};
    return std::string(payload.constData(), static_cast<std::size_t>(payload.size()));
}
bool GeometryCacheFiles::write(std::string_view descriptor, std::string_view bytes) const {
    if (directory_.empty() || bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(maximumBytes))
        return cannotWrite();
    if (!QDir().mkpath(directoryName(directory_)))
        return cannotWrite();
    const auto path = fileName(directory_, descriptor);
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto header = "BEND-GEOMETRY-1\n" + digest(descriptor) + '\n' + digest(bytes) + "\n\n";
    if (file.open(QIODevice::WriteOnly) && file.write(header) == header.size() &&
        file.write(view(bytes)) == static_cast<qint64>(bytes.size()) && file.commit())
        return true;
    return cannotWrite();
}
} // namespace bending::project
