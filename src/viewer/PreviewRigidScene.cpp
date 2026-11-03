#include "PreviewRigidScene.h"
#include <Prs3d_Drawer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <algorithm>
#include <utility>

namespace bending::viewer {
namespace {
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
    const bool boundaryChange = object->Attributes()->FaceBoundaryDraw() != boundaries;
    if (boundaryChange) {
        object->Attributes()->SetFaceBoundaryDraw(boundaries);
        object->SetToUpdate(); // Hidden presentations must also rebuild when shown again.
    }
    if (object->DisplayMode() != mode)
        context_->SetDisplayMode(object, mode, false);
    if (entry.body.visible) {
        if (!context_->IsDisplayed(object))
            context_->Display(object, mode, -1, false);
        if (boundaryChange)
            context_->Redisplay(object, false);
    } else if (context_->IsDisplayed(object)) {
        context_->Erase(object, false);
    }
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
                   sameColors(old.body.colors, body.colors) &&
                   std::none_of(next.begin(), next.end(), [&](const auto &entry) {
                       return entry.presentation == old.presentation;
                   });
        });
        Handle(AIS_ColoredShape) object;
        if (found != entries_.end()) {
            object = found->presentation;
        } else {
            object = new AIS_ColoredShape(body.shape.Located(TopLoc_Location()));
            object->Attributes()->SetAutoTriangulation(false);
            object->Attributes()->SetFaceBoundaryDraw(boundaries);
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
