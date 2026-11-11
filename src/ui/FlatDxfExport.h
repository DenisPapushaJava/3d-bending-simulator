#pragma once

#include "bending/application/FlatPattern.h"
#include <QLineF>
#include <QString>
#include <vector>

namespace bending::ui {
struct FlatDxfOptions {
    bool includeBends = true;
    QString bendLayer = QStringLiteral("BENDING");
    int bendColor = 1; // AutoCAD Color Index (ACI), red by default.
};

bool validBendLayer(const QString &name);
// Clipped marking segments in the source flat XY frame, before DXF orientation.
std::vector<QLineF> dxfBendLines(const application::FlatPatternInfo &flat);
// Empty message indicates successful atomic publication.
QString writeFlatDxf(const QString &path, const application::FlatPatternInfo &flat,
                     const FlatDxfOptions &options);
} // namespace bending::ui
