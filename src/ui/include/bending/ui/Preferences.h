#pragma once
#include <QSettings>
#include <memory>
namespace bending::ui {
// Configure once in app composition; tests/development retain native settings.
void setPreferencesFile(const QString &path);
std::unique_ptr<QSettings> preferences();
} // namespace bending::ui
