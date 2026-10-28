#include "bending/ui/Preferences.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
namespace bending::ui {
namespace {
QString file;
QString libraryRoot(const QSettings &settings) {
#ifdef _WIN32
    // A native Windows QSettings name is a registry key, not a writable folder.
    if (settings.format() == QSettings::NativeFormat)
        return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
    return QFileInfo(settings.fileName()).absolutePath();
}
} // namespace
void setPreferencesFile(const QString &path) {
    file = path;
}
std::unique_ptr<QSettings> preferences() {
    return file.isEmpty() ? std::make_unique<QSettings>()
                          : std::make_unique<QSettings>(file, QSettings::IniFormat);
}
QString toolCatalogPath() {
    const auto settings = preferences();
    const auto path = settings->value(QStringLiteral("tools/catalogPath")).toString();
    return path.isEmpty() || QFileInfo(path).isAbsolute()
               ? path
               : QDir(libraryRoot(*settings)).filePath(path);
}
QString toolLibraryDirectory() {
    return libraryRoot(*preferences());
}
} // namespace bending::ui
