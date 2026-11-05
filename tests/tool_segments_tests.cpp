#include "bending/core/ToolSegments.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
using namespace bending::core;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        std::stop_source stop;
        stop.request_stop();
        check(planToolSegments({{40, 3}}, 80, 200000, 3, stop.get_token()).status ==
                  SegmentPlanStatus::Cancelled,
              "A cancelled planner returned stock or a search-limit verdict");
        check(planToolSegments({{835, 1}}, 80).lengthMm == 835,
              "A longer tool was rejected for an unrestricted short bend");
        const auto nongreedy = planToolSegments({{8, 1}, {7, 1}, {6, 1}}, 13);
        check(nongreedy.status == SegmentPlanStatus::Complete && nongreedy.lengthMm == 13 &&
                  nongreedy.assembly.pieces.size() == 2,
              "Greedy overshoot was preferred");
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b)
                for (unsigned c = 0; c < 4; ++c)
                    for (int target = 1; target <= 50; ++target) {
                        std::pair<int, unsigned> best{1000, 1000};
                        for (unsigned x = 0; x <= a; ++x)
                            for (unsigned y = 0; y <= b; ++y)
                                for (unsigned z = 0; z <= c; ++z) {
                                    const int length = static_cast<int>(3 * x + 5 * y + 8 * z);
                                    if (length >= target)
                                        best = std::min(best, {length, x + y + z});
                                }
                        const std::vector<ToolSegment> stock{{3, a}, {5, b}, {8, c}};
                        const auto p = planToolSegments(stock, target);
                        if (best.first == 1000)
                            check(p.status == SegmentPlanStatus::InsufficientStock,
                                  "Invented stock");
                        else
                            check(p.status == SegmentPlanStatus::Complete &&
                                      p.lengthMm == best.first &&
                                      p.assembly.pieces.size() == best.second &&
                                      assemblyLength(stock, p.assembly) == p.lengthMm,
                                  "DP differs from independent exhaustive optimum");
                        const auto g = planToolSegments(stock, target, 200000, 3);
                        if (best.first != 1000) {
                            check(g.status == SegmentPlanStatus::Complete &&
                                      g.assembly.gapMm == 0 && g.lengthMm == best.first &&
                                      g.assembly.pieces.size() == best.second,
                                  "Gap fallback replaced a better gap-free optimum");
                        } else {
                            std::optional<std::tuple<int, int, unsigned>> expected;
                            for (unsigned x = 0; x <= a; ++x)
                                for (unsigned y = 0; y <= b; ++y)
                                    for (unsigned z = 0; z <= c; ++z) {
                                        const unsigned count = x + y + z;
                                        if (count < 2)
                                            continue;
                                        const int length =
                                            static_cast<int>(3 * x + 5 * y + 8 * z) * 1000;
                                        const int n = static_cast<int>(count);
                                        const int gapSize =
                                            (target * 1000 - length + n - 2) / (n - 1);
                                        if (gapSize > 3000)
                                            continue;
                                        const auto score =
                                            std::tuple{length + gapSize * (n - 1), gapSize, count};
                                        if (!expected || score < *expected)
                                            expected = score;
                                    }
                            if (!expected)
                                check(g.status == SegmentPlanStatus::InsufficientStock,
                                      "Invented gapped stock");
                            else
                                check(g.status == SegmentPlanStatus::Complete &&
                                          std::abs(g.lengthMm * 1000 - std::get<0>(*expected)) <
                                              1.e-6 &&
                                          std::abs(g.assembly.gapMm * 1000 -
                                                   std::get<1>(*expected)) < 1.e-6 &&
                                          g.assembly.pieces.size() == std::get<2>(*expected),
                                      "Gapped DP differs from independent exhaustive optimum");
                        }
                    }
        check(planToolSegments({{835, 0}, {100, 0}}, 80).status ==
                  SegmentPlanStatus::InsufficientStock,
              "A template was treated as stock");
        check(planToolSegments({{100, 1, SegmentShape::LeftHorn}}, 80).status ==
                  SegmentPlanStatus::InsufficientStock,
              "A horn was treated as the normal section");
        check(planToolSegments({{10, 100}}, 80, 1).status == SegmentPlanStatus::SearchLimit,
              "Search budget was ignored");
        check(!assemblyLength({{100, 1}}, {{0, 0}}) && !assemblyLength({{100, 1}}, {{1}}),
              "Stock overuse or foreign row accepted");
        const std::vector<ToolSegment> localStock{{20, 1}, {30, 1}, {40, 1}};
        const ToolAssembly local{{0, 1, 2}, 0, {0, 12.5}};
        const auto intervals = assemblyIntervals(localStock, local);
        check(assemblyLength(localStock, local) == 102.5 && intervals && intervals->size() == 3 &&
                  intervals->at(0).fromMm == 0 && intervals->at(0).toMm == 20 &&
                  intervals->at(1).fromMm == 20 && intervals->at(1).toMm == 50 &&
                  intervals->at(2).fromMm == 62.5 && intervals->at(2).toMm == 102.5 &&
                  maximumAssemblyGap(local) == 12.5 && assemblyGapAfter(local, 2) == 0,
              "A local window changed another joint or total span");
        check(assemblyUncoveredLength(localStock, local, 0, 50) == 0 &&
                  assemblyUncoveredLength(localStock, local, 62.5, 102.5) == 0 &&
                  assemblyUncoveredLength(localStock, local, 45, 70) == 12.5 &&
                  assemblyUncoveredLength(localStock, local, -10, 5) == 10 &&
                  !assemblyUncoveredLength(localStock, local, 5, 1),
              "Axial support accounting filled a gap or lost an end");
        for (const auto &bad : std::vector<ToolAssembly>{
                 {{0, 1, 2}, 1, {0, 12.5}},
                 {{0, 1, 2}, 0, {1}},
                 {{0, 1, 2}, 0, {0, -1}},
                 {{0, 1, 2}, 0, {0, 1001}},
                 {{0, 1, 2}, 0, {0, .0001}},
                 {{0, 1, 2}, 0, {0, std::numeric_limits<double>::quiet_NaN()}},
                 {{0, 1, 2}, 0, {0, std::numeric_limits<double>::infinity()}},
                 {{0}, 0, {0}},
                 {{0, 0, 2}, 0, {0, 10}}})
            check(!assemblyLength(localStock, bad) && !assemblyIntervals(localStock, bad),
                  "Invalid per-joint layout or stock overuse accepted");
        for (unsigned x = 0; x < 4; ++x)
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned z = 0; z < 4; ++z) {
                    bool feasible = false;
                    for (unsigned a = 0; a <= x; ++a)
                        for (unsigned b = 0; b <= y; ++b)
                            for (unsigned c = 0; c <= z; ++c)
                                for (unsigned d = 0; d + a <= x; ++d)
                                    for (unsigned e = 0; e + b <= y; ++e)
                                        for (unsigned f = 0; f + c <= z; ++f) {
                                            const auto left = 7 * a + 11 * b + 17 * c;
                                            const auto right = 7 * d + 11 * e + 17 * f;
                                            feasible |= left >= 25 && left <= 32 && right >= 25 &&
                                                        right <= 34 && left + right >= 60;
                                        }
                    const std::vector<ToolSegment> stock{{7, x}, {11, y}, {17, z}};
                    const auto plan = planToolWindow(stock, {80, 25, 32, 46, 55}, 20);
                    check((plan.status == SegmentPlanStatus::Complete) == feasible,
                          "Two-sided window planner disagrees with exhaustive shared-stock "
                          "allocation");
                    if (feasible)
                        check(assemblyLength(stock, plan.assembly) == 80 &&
                                  assemblyUncoveredLength(stock, plan.assembly, 0, 25) == 0 &&
                                  assemblyUncoveredLength(stock, plan.assembly, 55, 80) == 0 &&
                                  assemblyUncoveredLength(stock, plan.assembly, 32, 46) == 14 &&
                                  maximumAssemblyGap(plan.assembly) <= 20,
                              "Window plan lost required material, crossed the obstacle or "
                              "invented stock");
                }
        check(planToolWindow({{10, 20}}, {100, 30, 40, 60, 70}, 20, 1).status ==
                      SegmentPlanStatus::SearchLimit &&
                  planToolWindow({{10, 20}}, {100, 30, 40, 60, 70}, 20, 200000, stop.get_token())
                          .status == SegmentPlanStatus::Cancelled &&
                  planToolWindow({{10, 20}}, {100, 30, 40, 60, 70}, 19).status ==
                      SegmentPlanStatus::InsufficientStock &&
                  planToolWindow({{10, 20}}, {100, 45, 40, 60, 70}, 30).status ==
                      SegmentPlanStatus::InvalidInput,
              "Window planner ignored cancellation, quota, clearance or input validity");
        const auto precise = planToolSegments({{80.001, 1}}, 80.0008);
        const auto gap = planToolSegments({{100, 2}}, 210, 200000, 10);
        check(gap.status == SegmentPlanStatus::Complete && gap.assembly.gapMm == 10 &&
                  gap.lengthMm == 210 && assemblyLength({{100, 2}}, gap.assembly) == 210,
              "Custom gap missing from the actual assembly span");
        check(planToolSegments({{100, 2}}, 204, 200000, 3).status ==
                      SegmentPlanStatus::InsufficientStock &&
                  planToolSegments({{100, 2}}, 203, 200000, 3).assembly.gapMm == 3,
              "Default gap limit was ignored");
        check(planToolSegments({{100, 2}, {835, 1}}, 210, 200000, 10).assembly.gapMm == 0,
              "A gapped station was preferred despite an available gap-free one");
        check(precise.status == SegmentPlanStatus::Complete &&
                  std::abs(precise.lengthMm - 80.001) < 1.e-8,
              "Fractional length rounded below the required length");
        std::cout << "Segment planner matches exhaustive optima and stock constraints\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
