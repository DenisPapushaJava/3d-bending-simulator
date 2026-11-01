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
} // namespace bending::ui
