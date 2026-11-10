#pragma once

#include "bending/core/UnfoldRule.h"
#include <QStringList>
#include <vector>

class QWidget;

namespace bending::ui {
struct MaterialPreset {
    QString name;
    core::SheetMaterial material;
    std::optional<double> k = {};
    QStringList grades;
    std::optional<int> tensileStrengthMpa = {};
};
std::vector<MaterialPreset> materialPresets();
QString materialName(const core::UnfoldSettings &);
void editMaterialPresets(QWidget *parent);
} // namespace bending::ui
