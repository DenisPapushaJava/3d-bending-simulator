#!/usr/bin/env python3
"""Bounded application bridge for the synthetic experiment. MIT; not a STEP solver."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def validate_request(data):
    if not isinstance(data, dict) or (type(data.get("schema")) is not int or data["schema"] != 1):
        raise ValueError("Unsupported FEM request")
    if not isinstance(data.get("id"), str) or not data["id"] or len(data["id"]) > 80:
        raise ValueError("Missing job identity")
    params = data.get("parameters")
    if not isinstance(params, dict) or set(params) != {"hole", "mesh", "unload"}:
        raise ValueError("Incomplete synthetic model parameters")
    if params["hole"] not in ("center", "offset6", "none") or params["mesh"] not in ("coarse", "medium", "fine"):
        raise ValueError("Unsupported synthetic case")
    if type(params["unload"]) is not bool:
        raise ValueError("Unload must be boolean")
    if not isinstance(data.get("context"), str) or len(data["context"]) > 4096:
        raise ValueError("Invalid context")
    return params


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    if (args.directory / "completion.json").exists() or (args.directory / "case").exists():
        raise ValueError("Job directory already contains results; create a new job")
    raw = (args.directory / "request.json").read_bytes()
    params = validate_request(json.loads(raw))
    request = json.loads(raw)
    scripts = Path(__file__).resolve().parent
    command = [sys.executable, "-u", str(scripts / "bend_hole.py"), str(args.directory / "case"), "--half-model"]
    if params["hole"] == "none":
        command += ["--no-hole"]
    elif params["hole"] == "offset6":
        command += ["--hole-offset", "6"]
    if params["mesh"] == "coarse":
        command += ["--mesh-size", "3", "--hole-size", "1.2", "--layers", "1"]
    elif params["mesh"] == "fine":
        command += ["--mesh-size", "1.5", "--hole-size", "0.5", "--layers", "3"]
    if params["unload"]:
        command += ["--unload"]
    print("Подготовка сетки и нелинейный расчёт; обычно несколько минут…", flush=True)
    subprocess.run(command, check=True)
    print("Проверка полей, реакций и подготовка 3D-анимации…", flush=True)
    from results import analyze
    from report import viewer
    parsed = analyze(args.directory / "case")
    result = parsed[0]
    if not all(result["numerical_checks"][key] for key in ("force_balance", "sampled_node_penetration")):
        raise ValueError("Numerical research budgets exceeded; inspect case/result.json")
    viewer(args.directory / "case", args.directory / "result.html", parsed,
           {"job_id": request["id"], "cad_context": request["context"]})
    formed = min(result["history"], key=lambda entry: abs(entry["time"]-1))
    completion = {"schema": 1, "id": request["id"], "request_sha256": hashlib.sha256(raw).hexdigest(),
                  "parameters": params, "status": "completed_synthetic_research",
                  "formed_force_N": formed["punch_force_N"],
                  "formed_angle_deg": result["formed_flange_angle_deg"],
                  "unloaded_angle_deg": result["final_flange_angle_deg"] if params["unload"] else None}
    # Publish the small completion manifest last; failures never leave one behind.
    temporary = args.directory / "completion.json.tmp"
    temporary.write_text(json.dumps(completion, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    temporary.replace(args.directory / "completion.json")
    print("Расчёт учебной пластины завершён. Производственная точность не установлена.", flush=True)


if __name__ == "__main__":
    main()
