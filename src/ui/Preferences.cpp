#include "bending/ui/Preferences.h"
#include "ToolSegmentColorPreferences.h"
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
viewer::ToolSegmentColors toolSegmentColors() {
    auto settings = preferences();
    settings->beginGroup("viewer/segmentColors");
    viewer::ToolSegmentColors result;
    for (const auto &key : settings->childKeys()) {
        bool valid = false;
        const auto length = key.toLongLong(&valid);
        const QColor color(settings->value(key).toString());
        if (valid && length > 0 && length <= 100000000 && color.isValid())
            result[length] = QColor(color.name(QColor::HexRgb));
    }
    return result;
}
bool setToolSegmentColor(double lengthMm, const QColor &color) {
    const auto key = viewer::toolSegmentColorKey(lengthMm);
    if (!key || !color.isValid())
        return false;
    auto settings = preferences();
    settings->setValue(QStringLiteral("viewer/segmentColors/%1").arg(*key),
                       color.name(QColor::HexRgb));
    settings->sync();
    return settings->status() == QSettings::NoError;
}
bool resetToolSegmentColor(double lengthMm) {
    const auto key = viewer::toolSegmentColorKey(lengthMm);
    if (!key)
        return false;
    auto settings = preferences();
    settings->remove(QStringLiteral("viewer/segmentColors/%1").arg(*key));
    settings->sync();
    return settings->status() == QSettings::NoError;
}
} // namespace bending::ui
