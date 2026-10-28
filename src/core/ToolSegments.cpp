#include "bending/core/ToolSegments.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>

namespace bending::core {
bool validToolSegments(const std::vector<ToolSegment> &stock) {
    if (stock.size() > 64)
        return false;
    std::set<std::pair<std::int64_t, SegmentShape>> unique;
    for (const auto &s : stock) {
        if (!std::isfinite(s.lengthMm) || s.lengthMm < 1 || s.lengthMm > 100000 ||
            std::abs(s.lengthMm * 1000 - std::round(s.lengthMm * 1000)) > 1.e-6 ||
            s.quantity > 1000 ||
            (s.shape != SegmentShape::Straight && s.shape != SegmentShape::LeftHorn &&
             s.shape != SegmentShape::RightHorn) ||
            !unique.emplace(std::llround(s.lengthMm * 1000), s.shape).second)
            return false;
    }
    return true;
}
std::optional<double> assemblyLength(const std::vector<ToolSegment> &stock,
                                     const ToolAssembly &assembly) {
    if (!validToolSegments(stock) || assembly.pieces.empty() || assembly.pieces.size() > 512 ||
        !std::isfinite(assembly.gapMm) || assembly.gapMm < 0 || assembly.gapMm > 1000 ||
        std::abs(assembly.gapMm * 1000 - std::round(assembly.gapMm * 1000)) > 1.e-6)
        return {};
    std::vector<unsigned> used(stock.size());
    double length = 0;
    for (const auto row : assembly.pieces) {
        if (row >= stock.size() || stock[row].shape != SegmentShape::Straight ||
            ++used[row] > stock[row].quantity)
            return {};
        length += stock[row].lengthMm;
    }
    length += assembly.gapMm * static_cast<double>(assembly.pieces.size() - 1);
    return length <= 100000 ? std::optional{length} : std::nullopt;
}
static SegmentPlan planWithoutGaps(const std::vector<ToolSegment> &stock, double required,
                                   std::size_t stateBudget) {
    if (!validToolSegments(stock) || !std::isfinite(required) || required < 1 ||
        required > 100000 || !stateBudget || stateBudget > 200000)
        return {};
    const auto target = static_cast<std::int64_t>(std::ceil(required * 1000 - 1.e-6));
    std::int64_t available = 0, longest = 0;
    for (const auto &s : stock)
        if (s.shape == SegmentShape::Straight && s.quantity) {
            const auto n = std::llround(s.lengthMm * 1000);
            available += n * s.quantity;
            longest = std::max<std::int64_t>(longest, n);
        }
    if (available < target)
        return {SegmentPlanStatus::InsufficientStock, {}, 0};
    const auto ceiling = std::min<std::int64_t>(100000000, target + longest - 1);
    struct Node {
        std::int64_t length;
        unsigned count;
        std::size_t previous, row;
        unsigned take;
    };
    std::vector<Node> nodes{{0, 0, 0, 0, 0}};
    std::map<std::int64_t, std::size_t> states{{0, 0}};
    for (std::size_t row = 0; row < stock.size(); ++row) {
        const auto &s = stock[row];
        if (s.shape != SegmentShape::Straight)
            continue;
        unsigned remaining = std::min(s.quantity, 512U);
        for (unsigned block = 1; remaining; block *= 2) {
            const unsigned take = std::min(block, remaining);
            remaining -= take;
            const auto delta = std::llround(s.lengthMm * 1000) * take;
            const std::vector<std::pair<std::int64_t, std::size_t>> before(states.begin(),
                                                                           states.end());
            for (const auto &[sum, previous] : before) {
                if (sum + delta > ceiling || nodes[previous].count + take > 512)
                    continue;
                const auto found = states.find(sum + delta);
                if (found != states.end() &&
                    nodes[found->second].count <= nodes[previous].count + take)
                    continue;
                if (nodes.size() >= stateBudget)
                    return {SegmentPlanStatus::SearchLimit, {}, 0};
                nodes.push_back({sum + delta, nodes[previous].count + take, previous, row, take});
                states[sum + delta] = nodes.size() - 1;
            }
        }
    }
    const auto best = states.lower_bound(target);
    if (best == states.end())
        return {SegmentPlanStatus::InsufficientStock, {}, 0};
    SegmentPlan result{SegmentPlanStatus::Complete, {}, static_cast<double>(best->first) / 1000.};
    for (auto n = best->second; n; n = nodes[n].previous)
        result.assembly.pieces.insert(result.assembly.pieces.end(), nodes[n].take, nodes[n].row);
    std::sort(result.assembly.pieces.begin(), result.assembly.pieces.end(), [&](auto a, auto b) {
        return std::pair{-stock[a].lengthMm, a} < std::pair{-stock[b].lengthMm, b};
    });
    return result;
}
SegmentPlan planToolSegments(const std::vector<ToolSegment> &stock, double required,
                             std::size_t budget, double maximumGap) {
    if (!std::isfinite(maximumGap) || maximumGap < 0 || maximumGap > 1000)
        return {};
    const auto noGaps = planWithoutGaps(stock, required, budget);
    if (maximumGap == 0 || noGaps.status != SegmentPlanStatus::InsufficientStock)
        return noGaps;
    const auto target = static_cast<std::int64_t>(std::ceil(required * 1000 - 1.e-6));
    const auto gapLimit = static_cast<std::int64_t>(std::floor(maximumGap * 1000 + 1.e-6));
    struct Node {
        std::int64_t sum;
        unsigned count;
        std::size_t previous, row;
        unsigned take;
    };
    std::vector<Node> nodes{{0, 0, 0, 0, 0}};
    std::map<std::pair<std::int64_t, unsigned>, std::size_t> states{{{0, 0}, 0}};
    for (std::size_t row = 0; row < stock.size(); ++row) {
        if (stock[row].shape != SegmentShape::Straight)
            continue;
        unsigned remaining = std::min(stock[row].quantity, 512U);
        for (unsigned block = 1; remaining; block *= 2) {
            const auto take = std::min(block, remaining);
            remaining -= take;
            const auto delta = std::llround(stock[row].lengthMm * 1000) * take;
            const std::vector<std::pair<std::pair<std::int64_t, unsigned>, std::size_t>> before(
                states.begin(), states.end());
            for (const auto &[key, previous] : before) {
                const auto sum = key.first + delta;
                const auto count = key.second + take;
                if (sum >= target || count > 512 || states.contains({sum, count}))
                    continue;
                if (nodes.size() >= budget)
                    return {SegmentPlanStatus::SearchLimit, {}, 0};
                nodes.push_back({sum, count, previous, row, take});
                states[{sum, count}] = nodes.size() - 1;
            }
        }
    }
    std::optional<std::tuple<std::int64_t, std::int64_t, unsigned>> best;
    std::size_t chosen = 0;
    std::int64_t chosenGap = 0;
    for (const auto &[key, index] : states) {
        const auto [sum, count] = key;
        if (count < 2)
            continue;
        const auto gap = (target - sum + count - 2) / (count - 1);
        const auto span = sum + gap * (count - 1);
        if (gap > gapLimit || span > 100000000)
            continue;
        const auto score = std::tuple{span - target, gap, count};
        if (!best || score < *best) {
            best = score;
            chosen = index;
            chosenGap = gap;
        }
    }
    if (!best)
        return {SegmentPlanStatus::InsufficientStock, {}, 0};
    SegmentPlan result{SegmentPlanStatus::Complete, {}, 0};
    for (auto n = chosen; n; n = nodes[n].previous)
        result.assembly.pieces.insert(result.assembly.pieces.end(), nodes[n].take, nodes[n].row);
    result.assembly.gapMm = static_cast<double>(chosenGap) / 1000.;
    result.lengthMm = *assemblyLength(stock, result.assembly);
    return result;
}
} // namespace bending::core
