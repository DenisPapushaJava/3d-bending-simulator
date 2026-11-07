#pragma once
#include "bending/ui/DrawingSheet.h"
void verifyRecordedDimensions(const bending::application::BendingDrawing &,
                              const bending::ui::DrawingSheetPlan &);
void drawingLayoutChecks();

void verifySameDrawingLayout(const bending::ui::DrawingSheetPlan &,
                             const bending::ui::DrawingSheetPlan &);

void drawingReferenceChecks();
