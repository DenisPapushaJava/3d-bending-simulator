#pragma once
#include <QSettings>
#include <memory>
namespace bending::ui {
// Configure once in app composition; tests/development retain native settings.
void setPreferencesFile(const QString &path);
std::unique_ptr<QSettings> preferences();
// Own library paths are relative to settings, so portable data survives relocation.
QString toolCatalogPath();
QString toolLibraryDirectory();
// PDF destination is a local preference; it does not change the model/project.
QString suggestedDrawingPdfPath(const QString &modelPath);
void rememberDrawingPdfDirectory(const QString &savedPdfPath);
} // namespace bending::ui
