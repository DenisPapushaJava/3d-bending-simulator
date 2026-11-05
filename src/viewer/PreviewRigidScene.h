#pragma once
#include <AIS_ColoredShape.hxx>
#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Quantity_Color.hxx>
#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

namespace bending::viewer {
struct RigidDisplayColor {
    TopoDS_Shape shape; // Subshape in the body's local coordinates.
    Quantity_Color color;
};
struct RigidDisplayBody {
    std::string id;
    TopoDS_Shape shape; // Immutable, already meshed; location is the current placement.
    Quantity_Color color;
    double transparency = 0;
    bool visible = true;
    std::vector<RigidDisplayColor> colors{};
    bool boundaries = true; // Global shaded-edge mode can only narrow this body policy.
};

// One bounded set of the current preview's rigid bodies, owned by its graphics context.
// Reuse requires the actual shared topology, orientation and appearance, never an article/name.
class PreviewRigidScene final {
  public:
    explicit PreviewRigidScene(Handle(AIS_InteractiveContext));
    ~PreviewRigidScene() noexcept;
    PreviewRigidScene(const PreviewRigidScene &) = delete;
    PreviewRigidScene &operator=(const PreviewRigidScene &) = delete;
    void update(std::vector<RigidDisplayBody>, int displayMode, bool boundaries);
    void setStyle(int displayMode, bool boundaries);
    void refresh();
    void clear();

  private:
    struct Entry {
        RigidDisplayBody body;
        Handle(AIS_ColoredShape) presentation;
    };
    void apply(const Entry &, int displayMode, bool boundaries);
    Handle(AIS_InteractiveContext) context_;
    std::vector<Entry> entries_;
    int displayMode_ = AIS_Shaded;
    bool boundaries_ = true;
};
} // namespace bending::viewer
