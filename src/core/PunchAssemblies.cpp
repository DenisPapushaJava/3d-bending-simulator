#include "bending/core/ToolAssemblyEdits.h"
#include "bending/core/ToolSegments.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

namespace bending::core {
namespace {
using Length = std::int64_t;
struct Work {
    std::size_t limit, used = 0;
    std::stop_token stop;
    bool limited = false;
    bool take(std::size_t reserve = 0) {
        if (stop.stop_requested())
            return false;
        if (used >= limit - std::min(limit, reserve)) {
            limited = true;
            return false;
        }
        ++used;
        return true;
    }
};
struct Node {
    Length sum;
    unsigned count;
    std::size_t previous, row;
    unsigned take;
};
struct Middle {
    std::vector<std::size_t> pieces;
    double gap = 0;
};
std::vector<std::size_t> pieces(const std::vector<Node> &nodes, std::size_t n) {
    std::vector<std::size_t> result;
    for (; n; n = nodes[n].previous)
        result.insert(result.end(), nodes[n].take, nodes[n].row);
    return result;
}
struct Ends {
    std::optional<std::size_t> left, right;
    Length length = 0;
    unsigned count = 0;
};
using Query = std::pair<Length, unsigned>; // Fixed ends; the straight stock is unchanged.
} // namespace
std::optional<PunchAssemblyEnds> punchAssemblyEnds(const std::vector<ToolSegment> &stock,
                                                   const ToolAssembly &assembly) {
    if (!assemblyLength(stock, assembly))
        return {};
    PunchAssemblyEnds result;
    if (stock[assembly.pieces.front()].shape == SegmentShape::LeftHorn)
        result.left = assembly.pieces.front();
    if (stock[assembly.pieces.back()].shape == SegmentShape::RightHorn)
        result.right = assembly.pieces.back();
    return result;
}
std::optional<ToolAssembly> withPunchEnd(const std::vector<ToolSegment> &stock,
                                         const ToolAssembly &source, bool right,
                                         std::optional<std::size_t> row) {
    if ((!source.pieces.empty() && !assemblyLength(stock, source)) ||
        (source.pieces.empty() &&
         (!validToolSegments(stock) || source.gapMm != 0 || !source.jointGapsMm.empty())))
        return {};
    const auto shape = right ? SegmentShape::RightHorn : SegmentShape::LeftHorn;
    if (row && (*row >= stock.size() || stock[*row].shape != shape || !stock[*row].horn ||
                !usableToolSegment(stock[*row]) || !stock[*row].quantity))
        return {};
    ToolAssembly next{source.pieces, 0, {}};
    for (std::size_t i = 1; i < source.pieces.size(); ++i)
        next.jointGapsMm.push_back(assemblyGapAfter(source, i - 1));
    if (!next.pieces.empty()) {
        const auto at = right ? next.pieces.size() - 1 : 0;
        if (stock[next.pieces[at]].shape == shape) {
            if (row)
                next.pieces[at] = *row;
            else {
                next.pieces.erase(next.pieces.begin() + static_cast<std::ptrdiff_t>(at));
                if (!next.jointGapsMm.empty())
                    next.jointGapsMm.erase(right ? next.jointGapsMm.end() - 1
                                                 : next.jointGapsMm.begin());
            }
            row.reset();
        }
    }
    if (row) {
        if (!next.pieces.empty())
            next.jointGapsMm.insert(right ? next.jointGapsMm.end() : next.jointGapsMm.begin(), 0);
        next.pieces.insert(right ? next.pieces.end() : next.pieces.begin(), *row);
    }
    if (!next.pieces.empty() && !assemblyLength(stock, next))
        return {};
    return next.pieces == source.pieces ? source : next;
}
PunchAssemblyPlans planPunchAssemblies(const std::vector<ToolSegment> &stock, double required,
                                       std::size_t budget, double maximumGap, std::stop_token stop,
                                       std::size_t maximumPlans,
                                       std::optional<PunchAssemblyEnds> fixedEnds,
                                       std::optional<double> diagnosticSpanCapMm) {
    PunchAssemblyPlans result;
    if (stop.stop_requested()) {
        result.status = SegmentPlanStatus::Cancelled;
        return result;
    }
    if (!validToolSegments(stock) || !std::isfinite(required) || required < 1 ||
        required > 100000 || !std::isfinite(maximumGap) || maximumGap < 0 || maximumGap > 1000 ||
        !budget || budget > 200000 || !maximumPlans || maximumPlans > 32 ||
        (diagnosticSpanCapMm && (!fixedEnds || !std::isfinite(*diagnosticSpanCapMm) ||
                                 *diagnosticSpanCapMm < 1 || *diagnosticSpanCapMm > 100000)))
        return result;
    std::vector<std::optional<std::size_t>> left{std::nullopt}, right{std::nullopt};
    if (fixedEnds) {
        const auto valid = [&](std::optional<std::size_t> row, SegmentShape shape) {
            return !row ||
                   (*row < stock.size() && stock[*row].shape == shape && stock[*row].quantity &&
                    stock[*row].horn && usableToolSegment(stock[*row]));
        };
        if (!valid(fixedEnds->left, SegmentShape::LeftHorn) ||
            !valid(fixedEnds->right, SegmentShape::RightHorn))
            return result;
        left = {fixedEnds->left};
        right = {fixedEnds->right};
    } else {
        for (std::size_t i = 0; i < stock.size(); ++i)
            if (stock[i].quantity && stock[i].horn && usableToolSegment(stock[i]))
                (stock[i].shape == SegmentShape::LeftHorn ? left : right).push_back(i);
    }
    if (!diagnosticSpanCapMm && left.size() == 1 && !left.front() && right.size() == 1 &&
        !right.front()) {
        const auto old = planToolSegments(stock, required, budget, maximumGap, stop);
        result.status = old.status;
        if (old.status == SegmentPlanStatus::Complete)
            result.plans.push_back(old);
        return result;
    }
    Work work{budget, 0, stop};
    const auto target = static_cast<Length>(std::ceil(required * 1000 - 1e-6));
    Length longest = 0;
    for (const auto &s : stock)
        if (s.shape == SegmentShape::Straight && s.quantity)
            longest = std::max<Length>(longest, std::llround(s.lengthMm * 1000));
    const auto ceiling = diagnosticSpanCapMm
                             ? static_cast<Length>(std::floor(*diagnosticSpanCapMm * 1000 + 1.e-6))
                             : std::min<Length>(100000000, target + longest - 1);
    const auto reserve = std::min<std::size_t>(4096, budget / 4);
    // The minimum-piece witness at each length matches the straight reference.
    std::vector<Node> nodes{{0, 0, 0, 0, 0}};
    std::map<Length, std::size_t> states{{0, 0}};
    bool built = true;
    for (std::size_t row = 0; row < stock.size() && built; ++row) {
        if (stock[row].shape != SegmentShape::Straight)
            continue;
        unsigned remaining = std::min(stock[row].quantity, 512U);
        for (unsigned block = 1; remaining && built; block *= 2) {
            const auto take = std::min(block, remaining);
            remaining -= take;
            const auto delta = std::llround(stock[row].lengthMm * 1000) * take;
            const std::vector<std::pair<Length, std::size_t>> before(states.begin(), states.end());
            for (const auto &[sum, previous] : before) {
                if (!work.take(reserve)) {
                    built = false;
                    break;
                }
                if (sum + delta > ceiling || nodes[previous].count + take > 512)
                    continue;
                const auto found = states.find(sum + delta);
                if (found != states.end() &&
                    nodes[found->second].count <= nodes[previous].count + take)
                    continue;
                nodes.push_back({sum + delta, nodes[previous].count + take, previous, row, take});
                states[sum + delta] = nodes.size() - 1;
            }
        }
    }
    std::map<Query, std::optional<Middle>> answers;
    std::vector<Ends> ends;
    for (auto l : left)
        for (auto r : right) {
            Ends e{l, r};
            for (auto row : {l, r})
                if (row) {
                    e.length += std::llround(stock[*row].lengthMm * 1000);
                    ++e.count;
                }
            if (e.length > 100000000)
                continue;
            ends.push_back(e);
            const Query query{e.length, e.count};
            if (answers.contains(query))
                continue;
            std::optional<Middle> middle;
            const auto first = diagnosticSpanCapMm
                                   ? states.upper_bound(ceiling - e.length)
                                   : states.lower_bound(std::max<Length>(0, target - e.length));
            for (auto at = first;
                 diagnosticSpanCapMm ? at != states.begin() : at != states.end();) {
                if (!work.take())
                    break;
                const auto candidate = diagnosticSpanCapMm ? --at : at++;
                const auto &node = nodes[candidate->second];
                if (node.count + e.count == 0 || node.count + e.count > 512 ||
                    candidate->first + e.length > 100000000)
                    continue;
                auto selected = pieces(nodes, candidate->second);
                std::sort(selected.begin(), selected.end(), [&](auto a, auto b) {
                    return std::pair{-stock[a].lengthMm, a} < std::pair{-stock[b].lengthMm, b};
                });
                double gap = 0;
                if (diagnosticSpanCapMm && node.count + e.count > 1) {
                    const auto joints = node.count + e.count - 1;
                    const auto missing = target - candidate->first - e.length;
                    if (missing > 0) {
                        const auto candidateGap = (missing + joints - 1) / joints;
                        if (candidateGap * joints + candidate->first + e.length <= ceiling &&
                            static_cast<double>(candidateGap) / 1000 <= maximumGap)
                            gap = static_cast<double>(candidateGap) / 1000;
                    }
                }
                middle = Middle{std::move(selected), gap};
                break;
            }
            answers.emplace(query, std::move(middle));
        }
    // A missing gap-free family may still be filled using the allowed joints.
    if (!diagnosticSpanCapMm && maximumGap > 0 &&
        std::any_of(answers.begin(), answers.end(), [](const auto &a) { return !a.second; })) {
        std::vector<Node> gapNodes{{0, 0, 0, 0, 0}};
        std::map<std::pair<Length, unsigned>, std::size_t> gaps{{{0, 0}, 0}};
        built = true;
        for (std::size_t row = 0; row < stock.size() && built; ++row) {
            if (stock[row].shape != SegmentShape::Straight)
                continue;
            unsigned remaining = std::min(stock[row].quantity, 512U);
            for (unsigned block = 1; remaining && built; block *= 2) {
                const auto take = std::min(block, remaining);
                remaining -= take;
                const auto delta = std::llround(stock[row].lengthMm * 1000) * take;
                const std::vector<std::pair<std::pair<Length, unsigned>, std::size_t>> before(
                    gaps.begin(), gaps.end());
                for (const auto &[key, previous] : before) {
                    if (!work.take(reserve)) {
                        built = false;
                        break;
                    }
                    const auto sum = key.first + delta;
                    const auto count = key.second + take;
                    if (sum >= target || count > 512 || gaps.contains({sum, count}))
                        continue;
                    gapNodes.push_back({sum, count, previous, row, take});
                    gaps[{sum, count}] = gapNodes.size() - 1;
                }
            }
        }
        const auto gapLimit = static_cast<Length>(std::floor(maximumGap * 1000 + 1e-6));
        for (auto &[query, answer] : answers) {
            if (answer)
                continue;
            std::optional<std::tuple<Length, Length, unsigned>> best;
            std::size_t index = 0;
            Length chosenGap = 0;
            for (const auto &[key, n] : gaps) {
                if (!work.take())
                    break;
                const auto sum = key.first + query.first;
                const auto count = key.second + query.second;
                if (sum >= target || count < 2 || count > 512)
                    continue;
                const auto gap = (target - sum + count - 2) / (count - 1);
                const auto span = sum + gap * (count - 1);
                if (gap > gapLimit || span > 100000000)
                    continue;
                const auto score = std::tuple{span - target, gap, count};
                if (!best || score < *best) {
                    best = score;
                    index = n;
                    chosenGap = gap;
                }
            }
            if (best)
                answer = Middle{pieces(gapNodes, index), static_cast<double>(chosenGap) / 1000};
        }
    }
    std::array<std::vector<SegmentPlan>, 4> groups;
    for (const auto &e : ends) {
        const auto &answer = answers.at({e.length, e.count});
        if (!answer)
            continue;
        auto selected = answer->pieces;
        if (e.left)
            selected.insert(selected.begin(), *e.left);
        if (e.right)
            selected.push_back(*e.right);
        ToolAssembly assembly{std::move(selected), answer->gap, {}};
        const auto length = assemblyLength(stock, assembly);
        if (!length || (diagnosticSpanCapMm ? *length > *diagnosticSpanCapMm + 1.e-6
                                            : *length + 1.e-6 < required))
            continue;
        const auto group = (e.left ? 1U : 0U) + (e.right ? 2U : 0U);
        groups[group].push_back({SegmentPlanStatus::Complete, std::move(assembly), *length});
    }
    const auto score = [&](const auto &p) {
        return std::tuple{p.assembly.gapMm > 0, p.lengthMm - required, p.assembly.gapMm,
                          p.assembly.pieces.size()};
    };
    for (auto &g : groups)
        std::stable_sort(g.begin(), g.end(), [&](const auto &a, const auto &b) {
            const auto first = score(a), second = score(b);
            return first != second ? first < second : a.assembly.pieces < b.assembly.pieces;
        });
    if (!groups[0].empty())
        result.plans.push_back(std::move(groups[0].front()));
    for (std::size_t i = 0;; ++i) {
        bool any = false;
        for (std::size_t g = 1; g < groups.size(); ++g)
            if (i < groups[g].size()) {
                any = true;
                if (result.plans.size() < maximumPlans)
                    result.plans.push_back(std::move(groups[g][i]));
                else
                    work.limited = true;
            }
        if (!any)
            break;
    }
    result.work = work.used;
    if (stop.stop_requested()) {
        result.plans.clear();
        result.status = SegmentPlanStatus::Cancelled;
    } else if (work.limited)
        result.status = SegmentPlanStatus::SearchLimit;
    else
        result.status = result.plans.empty() ? SegmentPlanStatus::InsufficientStock
                                             : SegmentPlanStatus::Complete;
    return result;
}
} // namespace bending::core
