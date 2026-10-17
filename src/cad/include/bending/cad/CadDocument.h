#pragma once

#include "bending/application/Document.h"

#include <memory>

namespace bending::cad {
using application::BoundsMm;
using application::DiagnosticSeverity;
using application::DocumentMetadata;
using application::ImportDiagnostic;
using application::ImportStage;
using application::PointMm;

class StepImporter;

// One validated solid in millimetres. Only the importer can create a document.
// The geometry is owned here; copying a mutable OCCT handle is not part of this API.
class CadDocument final : public application::Document {
  public:
    ~CadDocument() override;
    CadDocument(const CadDocument &) = delete;
    CadDocument &operator=(const CadDocument &) = delete;

    [[nodiscard]] const DocumentMetadata &metadata() const noexcept override;
    [[nodiscard]] const std::vector<application::FaceInfo> &faces() const noexcept override;

  private:
    struct Impl;
    explicit CadDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class StepImporter;
    friend class OcctDocumentAccess;
};
} // namespace bending::cad
