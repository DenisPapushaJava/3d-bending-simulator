#include "ToolFixtures.h"
#include "bending/core/ToolSelection.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace bending::core;
namespace {
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
ToolFlangeHint rectangle(double x, double y, double top) {
    return {0, 1, {{{x, y}, {x, top}, {x + 2, top}, {x + 2, y}}}};
}
} // namespace
int main() {
    try {
        auto a = tool_fixture::pair(16);
        auto wide = tool_fixture::fourWayDie({16, 22, 35, 50}, 30);
        wide.id = "a-wide";
        a.die.id = "z-narrow";
        ToolSelectionRequest request{2, std::numbers::pi / 2, 4, 40};
        const ToolCatalog catalog{a.punch, wide, a.die};
        const auto base = selectTools(catalog, request);
        require(!base.candidates.empty() && catalog[base.candidates.front().die].id == wide.id,
                "Baseline parameter tie did not prefer fixture ID");
        request.shapeHints = {rectangle(25, -8, -28)};
        const auto ranked = selectTools(catalog, request);
        require(ranked.shapeRanked && ranked.candidates.size() == base.candidates.size() &&
                    catalog[ranked.candidates.front().die].id == a.die.id,
                "Close opposite flange did not prefer narrow die or removed a candidate");
        auto widePair = a;
        widePair.die = wide;
        const auto wideRisk = toolShapeRisk(widePair, request.shapeHints, 2);
        require(wideRisk && *wideRisk > 0 && toolShapeRisk(a, request.shapeHints, 2) == 0,
                "Full four-way body was reduced to its active V");
        auto stock = catalog;
        for (auto &tool : stock)
            tool.segments = std::vector<ToolSegment>{{20, 2}};
        const auto stocked = selectTools(stock, request);
        require(stocked.shapeRanked && stock[stocked.candidates.front().die].id == a.die.id &&
                    stocked.candidates.front().punchAssembly &&
                    stocked.candidates.front().dieAssembly,
                "Shape ranking lost the selected inventory assemblies");
        auto unknown = a.die;
        unknown.id = "b-unknown";
        unknown.drawing.reset();
        const ToolCatalog mixed{a.punch, wide, unknown, a.die};
        auto plainRequest = request;
        plainRequest.shapeHints.clear();
        const auto mixedBase = selectTools(mixed, plainRequest),
                   mixedRanked = selectTools(mixed, request);
        for (std::size_t i = 0; i < mixedBase.candidates.size(); ++i)
            if (mixedBase.candidates[i].die == 2)
                require(mixedRanked.candidates[i].die == 2 && !mixedRanked.candidates[i].shapeRisk,
                        "Unknown geometry did not retain its neutral slot");
        auto turned = a;
        turned.die.drawing = rotateToolDrawing(*a.die.drawing, std::numbers::pi / 2);
        turned.die.grooveSettings = {{a.die.angleRadians, -std::numbers::pi / 2}};
        require(toolShapeRisk(turned, request.shapeHints, 2) == 0,
                "Active working rotation was not applied to the full die");
        auto relief = a;
        auto &edges = relief.die.drawing->edges;
        edges.resize(6);
        edges.back().end = {30, 0};
        const std::vector<DrawingPoint> path{{30, 0},    {30, -5},  {20, -5},   {20, -35},
                                             {30, -35},  {30, -40}, {-30, -40}, {-30, -35},
                                             {-20, -35}, {-20, -5}, {-30, -5},  {-30, 0}};
        for (std::size_t i = 1; i < path.size(); ++i)
            edges.push_back({path[i - 1], path[i], {}, 0});
        edges.push_back({path.back(), edges.front().start, {}, 0});
        relief.die.heightMm = 40;
        require(hasCompleteDrawingToolGeometry(relief) &&
                    toolProfileSize(*relief.die.drawing)->widthMm == 60 &&
                    toolShapeRisk(relief, request.shapeHints, 2) == 0,
                "Full contour relief was replaced by a width threshold");
        auto hook = tool_fixture::hook();
        auto straight = hook;
        straight.punch = tool_fixture::pair(16).punch;
        const std::vector<ToolFlangeHint> upper{rectangle(3, 21, 32)};
        require(toolShapeRisk(hook, upper, 2) && toolShapeRisk(straight, upper, 2) &&
                    *toolShapeRisk(hook, upper, 2) < *toolShapeRisk(straight, upper, 2),
                "Upper flange did not distinguish punch relief");
        auto shifted = widePair;
        for (auto *drawing : {&*shifted.punch.drawing, &*shifted.die.drawing})
            for (auto &edge : drawing->edges) {
                for (auto *p : {&edge.start, &edge.end}) {
                    p->x += 101;
                    p->y -= 53;
                }
                if (edge.center) {
                    edge.center->x += 101;
                    edge.center->y -= 53;
                }
            }
        require(toolShapeRisk(shifted, request.shapeHints, 2) == wideRisk,
                "DXF origin changed shape ranking");
        auto mirrored = request.shapeHints;
        for (auto &p : mirrored.front().section)
            p.x = -p.x;
        require(toolShapeRisk(widePair, mirrored, 2) == wideRisk,
                "Workpiece-side alternatives depended on global X");
        auto order = request.shapeHints;
        auto easy = rectangle(100, -8, -28);
        easy.bendIndex = 1;
        easy.neighbourBendIndex = 0;
        order.push_back(easy);
        require(toolShapeRisk(widePair, order, 2) == 0,
                "An avoidable neighbouring-bend order became an unavoidable risk");
        auto sparse = widePair;
        sparse.die.drawing.reset();
        require(!toolShapeRisk(sparse, request.shapeHints, 2), "Missing shape became clear");
        sparse = a;
        sparse.useDrawingGeometry = false;
        require(!toolShapeRisk(sparse, request.shapeHints, 2),
                "Legacy ideal mode ranked real contours");
        auto invalid = request.shapeHints;
        invalid.front().section.front().x = std::numeric_limits<double>::quiet_NaN();
        require(!toolShapeRisk(a, invalid, 2), "Non-finite hint was ranked");
        std::stop_source stop;
        stop.request_stop();
        require(!toolShapeRisk(a, request.shapeHints, 2, stop.get_token()),
                "Cancelled ranking continued");
        request.shapeHints = invalid;
        const auto neutral = selectTools(catalog, request);
        require(!neutral.shapeRanked && neutral.candidates.size() == base.candidates.size() &&
                    neutral.candidates.front().die == base.candidates.front().die,
                "Unknown hints changed the parameter fallback");
        require(screenTools({6, std::numbers::pi / 2, 8, 40}, a.punch, a.die, 0) ==
                    ToolRejection::NarrowOpening,
                "Shape ranking weakened minimum V");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
