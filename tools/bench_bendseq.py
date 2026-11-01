#!/usr/bin/env python3
"""Optional research probe; BendSeq is not an application/build dependency."""

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REVISION = "774f6b81c9356ae2a67d5a02064956cfaa7c9b23"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path, help="BendSeq checkout at the audited revision")
    args = parser.parse_args()
    repo = args.repository.resolve()
    revision = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if revision != REVISION:
        parser.error(f"expected BendSeq revision {REVISION}, got {revision}")
    if subprocess.run(
        ["git", "-C", str(repo), "diff", "--quiet", "HEAD", "--", "src", "examples"],
        check=False,
    ).returncode:
        parser.error("BendSeq source/examples differ from the audited revision")
    sys.path.insert(0, str(repo / "src"))
    from bendseq.model import load_job
    from bendseq.solver import Failure, HOLD_SIDES, evaluate_operation, solve

    def compare(file):
        job = load_job(file)
        if len(job.bends) > 5:
            raise ValueError("exhaustive reference is limited to five bends")
        started = time.perf_counter()
        analysis = solve(job, limit=5)
        dp_ms = (time.perf_counter() - started) * 1000
        evaluations = 0
        solutions = []

        def visit(mask, steps, cost):
            nonlocal evaluations
            if mask == (1 << len(job.bends)) - 1:
                tokens = tuple(f"{s.bend_id}:{s.hold_side}" for s in steps)
                solutions.append((*cost, tokens))
                return
            for bend in range(len(job.bends)):
                if mask & (1 << bend):
                    continue
                for hold in HOLD_SIDES:
                    evaluations += 1
                    result = evaluate_operation(job, mask, bend, hold)
                    if isinstance(result, Failure):
                        continue
                    last = steps[-1] if steps else None
                    visit(mask | (1 << bend), (*steps, result), (
                        cost[0] + int(last is not None and last.tool != result.tool),
                        cost[1] + int(last is not None and last.hold_side != hold),
                        cost[2] + result.handling_span_milli,
                    ))

        started = time.perf_counter()
        visit(0, (), (0, 0, 0))
        reference_ms = (time.perf_counter() - started) * 1000
        actual = [(
            s.tool_changes, s.hold_flips, s.handling_span_milli,
            tuple(f"{st.bend_id}:{st.hold_side}" for st in s.steps),
        ) for s in analysis.solutions]
        if actual != sorted(solutions)[:5]:
            raise RuntimeError(f"{file.name}: top five differ from exhaustive reference")
        print(json.dumps({
            "revision": revision, "case": file.name, "status": analysis.status,
            "dp_ms": round(dp_ms, 3), "reference_ms": round(reference_ms, 3),
            "dp_evaluations": analysis.evaluated_operations,
            "reference_evaluations": evaluations, "states": analysis.explored_states,
            "solutions": len(solutions), "top5_matches": True,
        }), flush=True)

    with tempfile.TemporaryDirectory(prefix="bendseq-probe-") as folder:
        zigzag = Path(folder) / "five-bend-zigzag.json"
        zigzag.write_text(json.dumps({
            "name": "Five bend reference", "units": "mm", "segments": [100] * 6,
            "bends": [{"id": f"B{i + 1}", "angle": 20 if i % 2 == 0 else -20,
                       "tool": "v16"} for i in range(5)],
            "tools": {"v16": {"min_leg": 5, "clearance": 0, "keep_outs": []}},
        }), encoding="utf-8")
        for file in (repo / "examples/hat-channel.json",
                     repo / "examples/impossible-collision.json", zigzag):
            compare(file)


if __name__ == "__main__":
    main()
