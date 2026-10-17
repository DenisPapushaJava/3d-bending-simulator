#include "bending/cad/CadDocument.h"

#include "CadDocumentData.h"
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
} // namespace bending::cad
