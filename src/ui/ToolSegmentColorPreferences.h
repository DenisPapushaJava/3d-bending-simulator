#pragma once
#include "bending/viewer/ToolSegmentColors.h"

namespace bending::ui {
viewer::ToolSegmentColors toolSegmentColors();
bool setToolSegmentColor(double lengthMm, const QColor &color);
bool resetToolSegmentColor(double lengthMm);
} // namespace bending::ui
