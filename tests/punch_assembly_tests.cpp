#include "bending/core/ToolSegments.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace bending::core;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main() {
    try {
        for (unsigned x = 0; x < 3; ++x)
            for (unsigned y = 0; y < 3; ++y)
                for (unsigned l = 0; l < 2; ++l)
                    for (unsigned r = 0; r < 2; ++r)
                        for (int target = 1; target <= 30; ++target)
                            for (int maxGap : {0, 2}) {
                                std::vector<ToolSegment> stock{
                                    {3, x},
                                    {7, y},
                                    {5, l, SegmentShape::LeftHorn, HornRelief{2, 2, 1}, "left"},
                                    {6, r, SegmentShape::RightHorn, HornRelief{2, 2, 1}, "right"}};
                                const auto family =
                                    planPunchAssemblies(stock, target, 200000, maxGap);
                                check(family.work <= 200000, "Shared work budget exceeded");
                                std::map<unsigned, std::tuple<double, double, unsigned>> expected,
                                    actual;
                                for (unsigned left = 0; left <= l; ++left)
                                    for (unsigned right = 0; right <= r; ++right) {
                                        std::optional<std::tuple<double, double, unsigned>> noGaps,
                                            gaps;
                                        for (unsigned a = 0; a <= x; ++a)
                                            for (unsigned b = 0; b <= y; ++b) {
                                                const auto count = a + b + left + right;
                                                if (!count)
                                                    continue;
                                                const auto sum =
                                                    3 * a + 7 * b + 5 * left + 6 * right;
                                                if (sum >= static_cast<unsigned>(target)) {
                                                    const auto score =
                                                        std::tuple{double(sum), 0., count};
                                                    if (!noGaps || score < *noGaps)
                                                        noGaps = score;
                                                } else if (count > 1) {
                                                    const auto gap =
                                                        std::ceil((target - sum) * 1000. /
                                                                  (count - 1)) /
                                                        1000.;
                                                    const auto length = sum + gap * (count - 1);
                                                    const auto score =
                                                        std::tuple{length, gap, count};
                                                    if (gap <= maxGap && (!gaps || score < *gaps))
                                                        gaps = score;
                                                }
                                            }
                                        if (noGaps || gaps)
                                            expected.emplace(left + 2 * right,
                                                             noGaps ? *noGaps : *gaps);
                                    }
                                for (const auto &p : family.plans) {
                                    unsigned ends = 0;
                                    std::vector<unsigned> used(stock.size());
                                    for (auto i : p.assembly.pieces) {
                                        ++used.at(i);
                                        ends |= stock[i].shape == SegmentShape::LeftHorn    ? 1U
                                                : stock[i].shape == SegmentShape::RightHorn ? 2U
                                                                                            : 0U;
                                    }
                                    check(assemblyLength(stock, p.assembly) == p.lengthMm &&
                                              p.lengthMm + 1e-6 >= target,
                                          "Invalid emitted assembly");
                                    for (std::size_t i = 0; i < stock.size(); ++i)
                                        check(used[i] <= stock[i].quantity, "Invented stock");
                                    check(actual
                                              .emplace(ends,
                                                       std::tuple{p.lengthMm, p.assembly.gapMm,
                                                                  static_cast<unsigned>(
                                                                      p.assembly.pieces.size())})
                                              .second,
                                          "Repeated end family");
                                }
                                check(family.status == (expected.empty()
                                                            ? SegmentPlanStatus::InsufficientStock
                                                            : SegmentPlanStatus::Complete),
                                      "False limit/no-fit");
                                check(actual.size() == expected.size(),
                                      "A reachable end family was omitted");
                                for (const auto &[key, want] : expected) {
                                    const auto got = actual.at(key);
                                    check(std::abs(std::get<0>(want) - std::get<0>(got)) < 1e-6 &&
                                              std::abs(std::get<1>(want) - std::get<1>(got)) <
                                                  1e-6 &&
                                              std::get<2>(want) == std::get<2>(got),
                                          "End planner differs from exhaustive stock optimum");
                                }
                                const auto old = planToolSegments(stock, target, 200000, maxGap);
                                if (old.status == SegmentPlanStatus::Complete)
                                    check(family.best().assembly == old.assembly,
                                          "Successful straight reference changed");
                            }
        std::vector<ToolSegment> mixed{
            {100, 1, SegmentShape::LeftHorn, HornRelief{27, 27, 9}, "L"},
            {100, 1, SegmentShape::RightHorn, HornRelief{27, 27, 9}, "R"},
            {40, 1},
            {100, 99, SegmentShape::LeftHorn}};
        const auto single = planPunchAssemblies(mixed, 80);
        check(single.status == SegmentPlanStatus::Complete && single.plans.size() == 3 &&
                  single.best().lengthMm == 100,
              "Whole horn not offered as an ordinary working length");
        check(planToolSegments(mixed, 80).status == SegmentPlanStatus::InsufficientStock,
              "Straight reference started using horns");
        const auto baseWork = planPunchAssemblies(mixed, 80).work;
        mixed.insert(mixed.begin() + 1,
                     {100, 1, SegmentShape::LeftHorn, HornRelief{25, 27, 9}, "L2"});
        const auto expanded = planPunchAssemblies(mixed, 80, 200000, 0, {}, 32);
        check(expanded.work == baseWork, "Equal end lengths rebuilt the middle table/query");
        const auto fractional =
            planPunchAssemblies({{3.003, 1},
                                 {5.001, 1, SegmentShape::LeftHorn, HornRelief{2, 2, 1}, "fL"},
                                 {6.002, 1, SegmentShape::RightHorn, HornRelief{2, 2, 1}, "fR"}},
                                14.007, 200000, .001);
        check(fractional.status == SegmentPlanStatus::Complete && fractional.plans.size() == 1 &&
                  std::abs(fractional.best().lengthMm - 14.008) < 1e-9 &&
                  fractional.best().assembly.gapMm == .001,
              "Fractional lengths or the two end joints were rounded incorrectly");
        const auto tooMany = planPunchAssemblies(
            {{1, 512}, {2, 1, SegmentShape::LeftHorn, HornRelief{.5, .5, .5}, "limit"}}, 514);
        check(tooMany.status == SegmentPlanStatus::InsufficientStock,
              "Ends bypassed the 512-piece limit");
        const auto capped = planPunchAssemblies(mixed, 80, 200000, 0, {}, 3);
        check(capped.status == SegmentPlanStatus::SearchLimit && capped.plans.size() == 3,
              "Truncated geometry family declared complete");
        unsigned types = 0;
        for (const auto &p : capped.plans) {
            unsigned type = 0;
            for (auto i : p.assembly.pieces)
                type |= mixed[i].shape == SegmentShape::LeftHorn    ? 1U
                        : mixed[i].shape == SegmentShape::RightHorn ? 2U
                                                                    : 0U;
            types |= 1U << type;
        }
        check(types == ((1U << 1) | (1U << 2) | (1U << 3)), "Variants starved one end family");
        std::stop_source stop;
        stop.request_stop();
        check(planPunchAssemblies(mixed, 200, 200000, 0, stop.get_token()).status ==
                      SegmentPlanStatus::Cancelled &&
                  planPunchAssemblies(mixed, 200, 0).status == SegmentPlanStatus::InvalidInput &&
                  planPunchAssemblies(mixed, 200, 1).status == SegmentPlanStatus::SearchLimit,
              "Cancellation/budget ignored");
        std::cout << "Punch end families match exhaustive stock/gap optima and preserve straight "
                     "answers\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
