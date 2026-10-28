#include "bending/ui/Preferences.h"
namespace bending::ui {
namespace {
QString file;
}
void setPreferencesFile(const QString &path) {
    file = path;
}
std::unique_ptr<QSettings> preferences() {
    return file.isEmpty() ? std::make_unique<QSettings>()
                          : std::make_unique<QSettings>(file, QSettings::IniFormat);
}
} // namespace bending::ui
