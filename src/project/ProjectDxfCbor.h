#pragma once

#include "bending/application/ProjectDxf.h"
#include <QCborMap>

namespace bending::project {
QCborMap writeProjectDxf(const application::ProjectDxf &, const QCborMap &inputs,
                         const std::filesystem::path &project);
application::ProjectDxf readProjectDxf(const QCborValue &, const QCborMap &inputs,
                                       const std::filesystem::path &project);
} // namespace bending::project
