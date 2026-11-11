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
namespace {
constexpr auto pdfDirectory = "drawing/lastPdfDirectory";
constexpr auto dxfDirectory = "flatDxf/lastDirectory";
QString suggestedExportPath(const QString &modelPath, const QString &label,
                            const QString &extension, const char *directoryKey) {
    const QFileInfo source(modelPath);
    auto name = source.completeBaseName().trimmed();
    for (auto &c : name)
        if (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c))
            c = QLatin1Char('_');
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("Модель");
    auto filename = name + QStringLiteral(" — ") + label + extension;
    if (filename.size() > 255 || filename.toUtf8().size() > 255)
        filename = name + extension;
    const auto available = [](const QString &path) {
        const QFileInfo folder(path);
        return !path.isEmpty() && folder.isAbsolute() && folder.isDir() && folder.isReadable() &&
               folder.isWritable();
    };
    const auto saved = preferences()->value(directoryKey).toString();
    for (const auto &folder :
         {saved, modelPath.isEmpty() ? QString{} : source.absolutePath(),
          QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), QDir::homePath()})
        if (available(folder))
            return QDir(folder).filePath(filename);
    return QDir(QDir::homePath()).filePath(filename);
}
void rememberExportDirectory(const QString &path, const char *directoryKey) {
    const QFileInfo saved(path);
    if (!saved.isAbsolute() || !saved.isFile())
        return;
    auto settings = preferences();
    settings->setValue(directoryKey, saved.absolutePath());
    settings->sync();
}
} // namespace
QString suggestedDrawingPdfPath(const QString &modelPath) {
    return suggestedExportPath(modelPath, QStringLiteral("чертёж"), QStringLiteral(".pdf"),
                               pdfDirectory);
}
void rememberDrawingPdfDirectory(const QString &savedPdfPath) {
    rememberExportDirectory(savedPdfPath, pdfDirectory);
}
QString suggestedFlatDxfPath(const QString &modelPath) {
    return suggestedExportPath(modelPath, QStringLiteral("развёртка"), QStringLiteral(".dxf"),
                               dxfDirectory);
}
void rememberFlatDxfDirectory(const QString &savedDxfPath) {
    rememberExportDirectory(savedDxfPath, dxfDirectory);
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
