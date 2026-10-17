#include "bending/cad/CadDocument.h"

#include "CadDocumentData.h"

#include <utility>

namespace bending::cad {
CadDocument::CadDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CadDocument::~CadDocument() = default;

const DocumentMetadata &CadDocument::metadata() const noexcept {
    return impl_->metadata;
}
} // namespace bending::cad
