#pragma once

#include "bending/application/ProjectDrawingSettings.h"
#include <QJsonValue>

namespace bending::project {
QJsonValue drawingSettingsJson(const std::optional<application::ProjectDrawingSettings> &);
bool readDrawingSettings(const QJsonValue &, std::optional<application::ProjectDrawingSettings> &);
} // namespace bending::project
