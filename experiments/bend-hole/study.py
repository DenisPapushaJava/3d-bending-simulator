#!/usr/bin/env python3
"""Run the bounded, synthetic FEM experiment; no production CAD files. MIT."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys


CASES = {
    "beam": ["--no-hole", "--elastic", "--stroke", "0.01", "--width", "4", "--max-increment", ".1"],
    "control": ["--no-hole"],
    "center-coarse": ["--mesh-size", "3", "--hole-size", "1.2", "--layers", "1"],
    "center-medium": [],
    "center-fine": ["--mesh-size", "1.5", "--hole-size", ".5", "--layers", "3"],
    "offset": ["--hole-offset", "6"],
    "small-step": ["--max-increment", ".0125"],
    "stiff-contact": ["--penalty", "200000"],
    "unloading": ["--unload"],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    chosen = args.case or ["beam", "control", "center-coarse", "center-medium", "center-fine", "offset"]
    if any((args.output / name).exists() for name in chosen):
        parser.error("A selected case already exists; choose a fresh output directory")

    def run(name):
        command = [sys.executable, str(Path(__file__).with_name("bend_hole.py")),
                   str(args.output / name), "--half-model", *CASES[name]]
        with (args.output / f"{name}.log").open("w", encoding="utf-8") as log:
            code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
        row = {"case": name, "exit_code": code, "command": command}
        print(json.dumps(row), flush=True)
        return row

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(run, chosen))
    previous = args.output / "study-run.json"
    existing = json.loads(previous.read_text(encoding="utf-8")) if previous.exists() else []
    previous.write_text(json.dumps(existing + rows, indent=2) + "\n", encoding="utf-8")
    if any(row["exit_code"] for row in rows):
        raise SystemExit("Some cases failed: their results must not be used as completed predictions")


if __name__ == "__main__":
    main()
