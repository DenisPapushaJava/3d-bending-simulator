#include "PreviewRigidScene.h"
#include <Prs3d_Drawer.hxx>
#include <Select3D_SensitiveEntity.hxx>
#include <SelectMgr_Selection.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <algorithm>
#include <utility>

namespace bending::viewer {
namespace {
class ToolPresentation final : public AIS_ColoredShape {
  public:
    using AIS_ColoredShape::AIS_ColoredShape;

  protected:
    void ComputeSelection(const Handle(SelectMgr_Selection) &selection, int mode) override {
        AIS_ColoredShape::ComputeSelection(selection, mode);
        if (mode != AIS_Shape::SelectionMode(TopAbs_FACE))
            return;
        // Keep the face for resolving the clicked section. OCCT highlights its whole
        // presentation when the selection owner is marked as a global highlight.
        for (const auto &entity : selection->Entities())
            entity->BaseSensitive()->OwnerId()->SetComesFromDecomposition(false);
    }
};

bool isTool(const std::string &id) {
    return id == "punch" || id == "die";
}
bool sameTransform(const gp_Trsf &a, const gp_Trsf &b) {
    for (int row = 1; row <= 3; ++row)
        for (int column = 1; column <= 4; ++column)
            if (a.Value(row, column) != b.Value(row, column))
                return false;
    return true;
}
bool sameColors(const std::vector<RigidDisplayColor> &a, const std::vector<RigidDisplayColor> &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto &x, const auto &y) {
               return x.color.IsEqual(y.color) && x.shape.IsPartner(y.shape) &&
                      x.shape.Orientation() == y.shape.Orientation() &&
                      sameTransform(x.shape.Location().Transformation(),
                                    y.shape.Location().Transformation());
           });
}
} // namespace
PreviewRigidScene::PreviewRigidScene(Handle(AIS_InteractiveContext) context)
    : context_(std::move(context)) {}
PreviewRigidScene::~PreviewRigidScene() noexcept {
    try {
        clear();
    } catch (...) {
        // Native resources may already have gone away. The owning context is released next.
    }
}
void PreviewRigidScene::apply(const Entry &entry, int mode, bool boundaries) {
    const auto &object = entry.presentation;
    const auto &location = entry.body.shape.Location();
    if (!sameTransform(object->LocalTransformation(), location.Transformation()))
        context_->SetLocation(object, location);
    const bool drawBoundaries = boundaries && entry.body.boundaries;
    const bool boundaryChange = object->Attributes()->FaceBoundaryDraw() != drawBoundaries;
    if (boundaryChange) {
        object->Attributes()->SetFaceBoundaryDraw(drawBoundaries);
        object->SetToUpdate(); // Hidden presentations must also rebuild when shown again.
    }
    if (object->DisplayMode() != mode)
        context_->SetDisplayMode(object, mode, false);
    if (entry.body.visible) {
        if (!context_->IsDisplayed(object))
            context_->Display(object, mode,
                              toolPicking_ && isTool(entry.body.id)
                                  ? AIS_Shape::SelectionMode(TopAbs_FACE) : -1,
                              false);
        if (boundaryChange)
            context_->Redisplay(object, false);
    } else if (context_->IsDisplayed(object)) {
        context_->Erase(object, false);
    }
}
void PreviewRigidScene::setToolPicking(bool enabled) {
    if (toolPicking_ == enabled)
        return;
    if (!enabled)
        clearToolHover();
    toolPicking_ = enabled;
    const int mode = AIS_Shape::SelectionMode(TopAbs_FACE);
    for (const auto &entry : entries_) {
        if (!isTool(entry.body.id) || !context_->IsDisplayed(entry.presentation))
            continue;
        if (enabled) {
            // Presentations displayed with selection mode -1 are not loaded in the selector.
            context_->Load(entry.presentation, mode);
            context_->Activate(entry.presentation, mode, false);
        } else
            context_->Deactivate(entry.presentation, mode);
    }
}
void PreviewRigidScene::clearToolHover() {
    if (detectedTool())
        context_->ClearDetected(true);
}
std::optional<std::pair<bool, int>> PreviewRigidScene::detectedTool() const {
    if (!toolPicking_ || !context_->HasDetected())
        return {};
    const auto object = context_->DetectedInteractive();
    for (const auto &entry : entries_) {
        if (entry.presentation != object || !isTool(entry.body.id))
            continue;
        const auto owner = Handle(StdSelect_BRepOwner)::DownCast(context_->DetectedOwner());
        if (!owner.IsNull() && owner->HasShape()) {
            const auto &face = owner->Shape();
            for (std::size_t i = 0; i < entry.body.colors.size(); ++i)
                for (TopExp_Explorer faces(entry.body.colors[i].shape, TopAbs_FACE); faces.More();
                     faces.Next())
                    if (face.IsSame(faces.Current()))
                        return std::pair{entry.body.id == "punch", static_cast<int>(i)};
        }
        return std::pair{entry.body.id == "punch", -1};
    }
    return {};
}
void PreviewRigidScene::update(std::vector<RigidDisplayBody> bodies, int mode, bool boundaries) {
    std::vector<Entry> next;
    next.reserve(bodies.size());
    for (auto &body : bodies) {
        if (body.shape.IsNull())
            continue;
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto &old) {
            return old.body.id == body.id && old.body.shape.IsPartner(body.shape) &&
                   old.body.shape.Orientation() == body.shape.Orientation() &&
                   old.body.color.IsEqual(body.color) &&
                   old.body.transparency == body.transparency &&
                   old.body.boundaries == body.boundaries &&
                   sameColors(old.body.colors, body.colors) &&
                   std::none_of(next.begin(), next.end(), [&](const auto &entry) {
                       return entry.presentation == old.presentation;
                   });
        });
        Handle(AIS_ColoredShape) object;
        if (found != entries_.end()) {
            object = found->presentation;
        } else {
            const auto localShape = body.shape.Located(TopLoc_Location());
            if (isTool(body.id))
                object = new ToolPresentation(localShape);
            else
                object = new AIS_ColoredShape(localShape);
            object->Attributes()->SetAutoTriangulation(false);
            object->Attributes()->SetFaceBoundaryDraw(boundaries && body.boundaries);
            object->SetColor(body.color);
            if (body.transparency > 0)
                object->SetTransparency(body.transparency);
            for (const auto &group : body.colors) {
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(group.shape, TopAbs_FACE, faces);
                for (int face = 1; face <= faces.Extent(); ++face)
                    object->SetCustomColor(faces(face), group.color);
            }
        }
        next.push_back({std::move(body), object});
    }
    try {
        for (const auto &entry : next)
            apply(entry, mode, boundaries);
        for (const auto &old : entries_)
            if (std::none_of(next.begin(), next.end(), [&](const auto &entry) {
                    return entry.presentation == old.presentation;
                }))
                context_->Remove(old.presentation, false);
    } catch (...) {
        // Keep the previously accepted frame if an SDK presentation update fails.
        try {
            for (const auto &entry : next)
                if (std::none_of(entries_.begin(), entries_.end(), [&](const auto &old) {
                        return entry.presentation == old.presentation;
                    }))
                    context_->Remove(entry.presentation, false);
            for (const auto &old : entries_)
                apply(old, displayMode_, boundaries_);
        } catch (...) {
            // The canvas reports the original failure and can recreate its native context.
        }
        throw;
    }
    entries_ = std::move(next);
    displayMode_ = mode;
    boundaries_ = boundaries;
}
void PreviewRigidScene::setStyle(int mode, bool boundaries) {
    for (const auto &entry : entries_)
        apply(entry, mode, boundaries);
    displayMode_ = mode;
    boundaries_ = boundaries;
}
void PreviewRigidScene::refresh() {
    for (const auto &entry : entries_)
        if (entry.body.visible)
            context_->RecomputePrsOnly(entry.presentation, false, true);
}
void PreviewRigidScene::clear() {
    for (const auto &entry : entries_)
        context_->Remove(entry.presentation, false);
    entries_.clear();
}
} // namespace bending::viewer
