#pragma once
#include "bending/core/ToolCatalog.h"
#include <QJsonObject>

namespace bending::project {
QJsonObject presetJson(const core::ToolPreset &preset);
bool readPreset(const QJsonObject &object, core::ToolPreset &preset);
QJsonValue assignmentJson(const std::optional<core::ToolAssignment> &assignment);
bool readAssignment(const QJsonValue &value, std::optional<core::ToolAssignment> &assignment);
} // namespace bending::project
