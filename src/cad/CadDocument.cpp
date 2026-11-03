#include "bending/cad/CadDocument.h"

#include "BendRecognizer.h"
#include "CadDocumentData.h"
#include "SheetOrientation.h"
#include "bending/cad/occt/DocumentAccess.h"

#include <utility>

namespace bending::cad {
CadDocument::CadDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CadDocument::~CadDocument() = default;

const DocumentMetadata &CadDocument::metadata() const noexcept {
    return impl_->metadata;
}

const std::vector<application::FaceInfo> &CadDocument::faces() const noexcept {
    return impl_->faces;
}

std::string_view CadDocument::sourceStep() const noexcept {
    return impl_->sourceStep;
}

const TopoDS_Shape &OcctDocumentAccess::shape(const CadDocument &document) {
    return document.impl_->shape;
}
const TopoDS_Shape &OcctDocumentAccess::simulationShape(const CadDocument &document) {
    return document.impl_->filledShape.IsNull() ? document.impl_->shape
                                                : document.impl_->filledShape;
}
const application::BendAnalysis &OcctDocumentAccess::simulationBends(const CadDocument &document) {
    return document.impl_->filledShape.IsNull() ? document.impl_->metadata.bendAnalysis
                                                : document.impl_->filledBends;
}
const TopoDS_Shape &OcctDocumentAccess::routeSkin(const CadDocument &document) {
    return document.impl_->routeSkin;
}
bool OcctDocumentAccess::isRouteState(const CadDocument &document) {
    return document.impl_->routeState;
}
const SheetOrientationData &OcctDocumentAccess::sheetOrientation(const CadDocument &document) {
    return document.impl_->orientation;
}
const std::vector<SheetSide> &OcctDocumentAccess::routeSkinSides(const CadDocument &document) {
    return document.impl_->routeSkinSides;
}
std::shared_ptr<const CadDocument>
OcctDocumentAccess::routeState(const CadDocument &original, TopoDS_Shape shape, TopoDS_Shape skin,
                               std::stop_token stop, std::optional<SheetReference> reference,
                               std::vector<SheetSide> sides) {
    auto data = std::make_unique<CadDocument::Impl>();
    data->metadata = original.metadata();
    data->metadata.bendAnalysis = recognizeBends(shape, data->metadata.maximumToleranceMm, stop);
    data->metadata.toolShapeHints.clear(); // Source-only hints must not follow internal bend IDs.
    data->shape = std::move(shape);
    data->routeSkin = std::move(skin);
    data->routeState = true;
    data->routeSkinSides = std::move(sides);
    if (reference)
        data->orientation = classifySheetOrientation(data->shape, data->metadata, stop, reference);
    return std::shared_ptr<const CadDocument>(new CadDocument(std::move(data)));
}
} // namespace bending::cad
