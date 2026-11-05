#include "bending/core/ToolSegments.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

namespace bending::core {
SegmentPlan planToolWindow(const std::vector<ToolSegment> &stock, const ToolWindow &w,
                           double maximumGap, std::size_t workBudget, std::stop_token stop) {
    if (stop.stop_requested())
        return {SegmentPlanStatus::Cancelled, {}, 0};
    if (!validToolSegments(stock) || !workBudget || workBudget > 200000 ||
        !std::isfinite(maximumGap) || maximumGap < 0 || maximumGap > 1000 ||
        !std::isfinite(w.lengthMm) || !std::isfinite(w.freeFromMm) ||
        !std::isfinite(w.conflictFromMm) || !std::isfinite(w.conflictToMm) ||
        !std::isfinite(w.freeToMm) || w.lengthMm > 100000 ||
        !(0 < w.freeFromMm && w.freeFromMm <= w.conflictFromMm &&
          w.conflictFromMm < w.conflictToMm && w.conflictToMm <= w.freeToMm &&
          w.freeToMm < w.lengthMm) ||
        std::abs(w.lengthMm * 1000 - std::round(w.lengthMm * 1000)) > 1.e-6)
        return {};
    using Int = std::int64_t;
    const Int length = std::llround(w.lengthMm * 1000);
    const Int minLeft = static_cast<Int>(std::ceil(w.freeFromMm * 1000 - 1.e-6));
    const Int maxLeft = static_cast<Int>(std::floor(w.conflictFromMm * 1000 + 1.e-6));
    const Int minRight = static_cast<Int>(std::ceil((w.lengthMm - w.freeToMm) * 1000 - 1.e-6));
    const Int maxRight = static_cast<Int>(std::floor((w.lengthMm - w.conflictToMm) * 1000 + 1.e-6));
    const Int maxGap = static_cast<Int>(std::floor(maximumGap * 1000 + 1.e-6));
    if (minLeft > maxLeft || minRight > maxRight || length - maxLeft - maxRight > maxGap)
        return {SegmentPlanStatus::InsufficientStock, {}, 0};
    Int available = 0;
    for (const auto &s : stock)
        if (s.shape == SegmentShape::Straight)
            available += std::llround(s.lengthMm * 1000) * std::min(s.quantity, 512u);
    if (available < std::max(minLeft + minRight, length - maxGap))
        return {SegmentPlanStatus::InsufficientStock, {}, 0};
    struct Node {
        Int left, right;
        std::size_t previous, row;
        unsigned count;
        bool onLeft;
    };
    std::vector<Node> nodes{{0, 0, 0, 0, 0, false}};
    std::map<std::pair<Int, Int>, std::size_t> states{{{0, 0}, 0}};
    std::size_t work = 0;
    for (std::size_t row = 0; row < stock.size(); ++row) {
        const auto &s = stock[row];
        const Int piece = std::llround(s.lengthMm * 1000);
        if (s.shape != SegmentShape::Straight || piece > std::max(maxLeft, maxRight))
            continue;
        for (unsigned take = 0; take < std::min(s.quantity, 512u); ++take) {
            std::vector<std::size_t> before;
            before.reserve(states.size());
            for (const auto &[sum, index] : states) {
                (void)sum;
                before.push_back(index);
            }
            for (const auto previous : before) {
                const auto n = nodes[previous];
                if (n.count >= 512)
                    continue;
                for (const bool left : {true, false}) {
                    if (stop.stop_requested())
                        return {SegmentPlanStatus::Cancelled, {}, 0};
                    if (++work > workBudget)
                        return {SegmentPlanStatus::SearchLimit, {}, 0};
                    const Int a = n.left + (left ? piece : 0);
                    const Int b = n.right + (left ? 0 : piece);
                    if (a > maxLeft || b > maxRight)
                        continue;
                    const auto found = states.find({a, b});
                    if (found != states.end() && nodes[found->second].count <= n.count + 1)
                        continue;
                    nodes.push_back({a, b, previous, row, n.count + 1, left});
                    const auto index = nodes.size() - 1;
                    states[{a, b}] = index;
                    if (a < minLeft || b < minRight || length - a - b > maxGap)
                        continue;
                    std::vector<std::size_t> first, second;
                    for (auto at = index; at; at = nodes[at].previous)
                        (nodes[at].onLeft ? first : second).push_back(nodes[at].row);
                    std::reverse(first.begin(), first.end());
                    std::reverse(second.begin(), second.end());
                    ToolAssembly result;
                    result.pieces = std::move(first);
                    const auto joint = result.pieces.size() - 1;
                    result.pieces.insert(result.pieces.end(), second.begin(), second.end());
                    result.jointGapsMm.resize(result.pieces.size() - 1);
                    result.jointGapsMm[joint] = static_cast<double>(length - a - b) / 1000;
                    return {SegmentPlanStatus::Complete, std::move(result), w.lengthMm};
                }
            }
        }
    }
    return {SegmentPlanStatus::InsufficientStock, {}, 0};
}
} // namespace bending::core
