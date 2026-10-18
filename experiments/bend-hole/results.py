"""Read CalculiX ASCII output without modifying or completing missing results. MIT."""

from __future__ import annotations

import json
import math
from pathlib import Path
import re

from bend_hole import Model, boundary, parse_mesh


def tool_outline(model, name, subdivisions=48):
    """Independent sampled 2D outline for a nodal penetration check, not contact solving."""
    if name == "PUNCH":
        beta = math.radians(model.punch_angle / 2)
        r = model.nose_radius
        center_y = model.thickness + r
        tangent_y = center_y - r * math.sin(beta)
        x = r * math.cos(beta)
        top_y = model.thickness + 12
        top_x = x + (top_y - tangent_y) * math.tan(beta)
        vertices = [(-top_x, top_y), (-x, tangent_y), (0, model.thickness),
                    (x, tangent_y), (top_x, top_y)]
        arcs = {1: (0, center_y), 2: (0, center_y)}
    else:
        a, r = model.opening / 2, model.shoulder_radius
        d = r * math.tan(math.pi / 8)
        q = d / math.sqrt(2)
        vertices = [(-35, 0), (-a-d, 0), (-a+q, -q), (0, -a), (a-q, -q),
                    (a+d, 0), (35, 0), (35, -20), (-35, -20)]
        arcs = {1: (-a-d, -r), 4: (a+d, -r)}
    points = []
    for i, first in enumerate(vertices):
        last = vertices[(i + 1) % len(vertices)]
        points.append(first)
        if i in arcs:
            cx, cy = arcs[i]
            start = math.atan2(first[1] - cy, first[0] - cx)
            end = math.atan2(last[1] - cy, last[0] - cx)
            delta = (end - start + math.pi) % (2 * math.pi) - math.pi
            for j in range(1, subdivisions):
                angle = start + delta * j / subdivisions
                points.append((cx + r * math.cos(angle), cy + r * math.sin(angle)))
    return points


def nodal_penetration(model, points, stroke):
    import numpy as np

    largest = 0.0
    for name in ("DIE", "PUNCH"):
        polygon = np.array(tool_outline(model, name))
        if name == "PUNCH":
            polygon[:, 1] -= stroke
        inside = np.zeros(len(points), dtype=bool)
        edges = list(zip(polygon, np.roll(polygon, -1, axis=0)))
        x, y = points.T
        for a, b in edges:
            if abs(b[1] - a[1]) > 1e-12:
                inside ^= ((a[1] > y) != (b[1] > y)) & (x < (b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0])
        candidates = points[inside]
        if not len(candidates):
            continue
        distance = np.full(len(candidates), np.inf)
        for a, b in edges:
            direction = b - a
            fraction = np.clip((candidates - a) @ direction / (direction @ direction), 0, 1)
            distance = np.minimum(distance, np.linalg.norm(candidates - a - fraction[:, None]*direction, axis=1))
        largest = max(largest, float(distance.max()))
    return largest


def read_frd(path, wanted_nodes):
    with path.open(encoding="utf-8") as stream:
        return _read_frd_records(stream, wanted_nodes)


def _read_frd_records(lines, wanted_nodes):
    frames = {}
    name, time, values = "", None, {}
    supported = {"DISP": 3, "STRESS": 6, "PE": 1}
    for line in lines:
        if line.lstrip().startswith("100C"):
            time = float(line.split()[2])
        elif line.startswith(" -4"):
            name = line.split()[1]
            values = {}
        elif line.startswith(" -1") and name in supported:
            node = int(line[3:13])
            if node in wanted_nodes:
                components = tuple(float(line[i:i + 12]) for i in range(13, len(line.rstrip()), 12))
                if len(components) != supported[name] or not all(map(math.isfinite, components)):
                    raise ValueError(f"Invalid FRD {name} record for node {node}")
                values[node] = components
        elif line.startswith(" -3") and name in supported:
            if time is None or not values:
                raise ValueError(f"Empty FRD {name} result")
            frames.setdefault(time, {})[name] = values
            name, values = "", {}
    if not frames:
        raise ValueError("No result frames")
    for time, frame in frames.items():
        if set(frame.get("DISP", {})) != wanted_nodes:
            raise ValueError(f"Incomplete displacement frame at {time}")
    return frames


def read_reactions(path, set_name="PUNCH_REF", displacements=True):
    frames = {}
    kind, time = None, None
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.search(r"(displacements|forces).*set " + re.escape(set_name) + r" and time\s+(\S+)", line)
        if match:
            kind, time = match[1], float(match[2])
        elif kind and line.strip():
            fields = line.split()
            values = tuple(map(float, fields[1:]))
            if len(values) != 3 or not all(map(math.isfinite, values)):
                raise ValueError("Invalid punch reference output")
            frames.setdefault(time, {})[kind] = values
            kind = None
    required = {"displacements", "forces"} if displacements else {"forces"}
    if not frames or any(set(frame) != required for frame in frames.values()):
        raise ValueError("Incomplete reference reaction history")
    return frames


def check_completion(directory, model):
    if "Job finished" not in (directory / "solver.log").read_text(encoding="utf-8"):
        raise ValueError("Solver did not finish")
    rows = []
    for line in (directory / "model.sta").read_text(encoding="utf-8").splitlines()[2:]:
        fields = line.split()
        if len(fields) == 7 and not fields[2].endswith("U"):
            rows.append(fields)
    if not rows:
        raise ValueError("No converged increments")
    expected_steps = 2 if model.unload else 1
    if int(rows[-1][0]) != expected_steps or abs(float(rows[-1][5]) - 1) > 1e-7:
        raise ValueError("The requested final increment was not reached")
    return len(rows)


def hole_metrics(model, nodes, displacement):
    import numpy as np

    if not model.hole_radius:
        return None
    ring = [n for n, (x, y, z) in nodes.items()
            if abs(y - model.thickness / 2) < 1e-7
            and abs(math.hypot(x - model.hole_offset, z) - model.hole_radius) < 1e-7]
    if len(ring) < (4 if model.half_model else 8) or any(n not in displacement for n in ring):
        raise ValueError("Missing mid-thickness hole contour")
    ring.sort(key=lambda n: math.atan2(0.0 if abs(nodes[n][2]) < 1e-7 else nodes[n][2],
                                      nodes[n][0] - model.hole_offset))
    original = np.array([nodes[n] for n in ring])
    points = np.array([np.array(nodes[n]) + displacement[n] for n in ring])
    if model.half_model:
        reflected = points[[i for i, n in enumerate(ring) if abs(nodes[n][2]) > 1e-7]].copy()
        reflected[:, 2] *= -1
        points = np.concatenate([points, reflected[::-1]])
        initial_reflected = original[[i for i, n in enumerate(ring) if abs(nodes[n][2]) > 1e-7]].copy()
        initial_reflected[:, 2] *= -1
        original = np.concatenate([original, initial_reflected[::-1]])
    centered = points - points.mean(axis=0)
    _, _, axes = np.linalg.svd(centered, full_matrices=False)
    projected = centered @ axes[:2].T
    span = np.ptp(projected, axis=0)
    perimeter = np.linalg.norm(points - np.roll(points, 1, axis=0), axis=1).sum()
    initial_perimeter = np.linalg.norm(original - np.roll(original, 1, axis=0), axis=1).sum()
    return {
        "source_ring_nodes": len(ring),
        "sampled_nodes": len(points),
        "best_fit_plane_spans_mm": sorted(span.tolist(), reverse=True),
        "max_distance_from_fit_plane_mm": float(np.max(np.abs(centered @ axes[2]))),
        "polygonal_3d_perimeter_mm": float(perimeter),
        "initial_polygonal_perimeter_mm": float(initial_perimeter),
        "perimeter_ratio": float(perimeter / initial_perimeter),
        "points_mm": points.tolist(),
        "interpretation": "Projected spans include overall bending; they are not intrinsic ovality.",
    }


def flange_angle(model, nodes, displacement):
    import numpy as np

    normals = []
    for side in (-1, 1):
        ids = [n for n, (x, y, z) in nodes.items() if n in displacement
               and side*x > model.opening/2+6 and abs(y-model.thickness/2) < 1e-7]
        if len(ids) < 6:
            raise ValueError("Missing far-flange samples")
        points = np.array([np.array(nodes[n])+displacement[n] for n in ids])
        _, _, axes = np.linalg.svd(points-points.mean(axis=0), full_matrices=False)
        normal = axes[2] if axes[2][1] >= 0 else -axes[2]
        normals.append(normal)
    return math.degrees(math.acos(float(np.clip(normals[0] @ normals[1], -1, 1))))


def analyze(directory):
    import numpy as np

    metadata = json.loads((directory / "input.json").read_text(encoding="utf-8"))
    model = Model(**metadata["model"])
    increments = check_completion(directory, model)
    version = re.search(r"CalculiX Version\s+([\d.]+)", (directory / "solver.log").read_text(encoding="utf-8"))
    if version is None or version[1] != "2.21":
        raise ValueError("This research reader expects the pinned CalculiX 2.21 executable")
    nodes, elements, groups = parse_mesh(directory / "mesh.inp")
    wanted = set(metadata["plate_nodes"])
    frames = read_frd(directory / "model.frd", wanted)
    reactions = read_reactions(directory / "model.dat")
    factor = 2 if model.half_model else 1
    die_reactions, anchor_reactions = None, None
    if "set DIE_REF" in (directory / "model.dat").read_text(encoding="utf-8"):
        die_reactions = read_reactions(directory / "model.dat", "DIE_REF", displacements=False)
        anchor_reactions = read_reactions(directory / "model.dat", "ANCHOR", displacements=False)
    weight = (model.length * model.width - math.pi * model.hole_radius ** 2) * model.thickness * 7.85e-9 * 9810
    history = []
    for time, frame in sorted(frames.items()):
        if set(frame.get("STRESS", {})) != wanted or (
            not model.elastic and set(frame.get("PE", {})) != wanted
        ):
            raise ValueError("Missing plate stress/plastic-strain field")
        nearest = min(reactions, key=lambda value: abs(value - time))
        if abs(nearest - time) > 1e-5:
            raise ValueError("Displacement/reaction time mismatch")
        reaction = reactions[nearest]
        stress = np.array(list(frame["STRESS"].values()))
        sxx, syy, szz, sxy, syz, szx = stress.T
        mises = np.sqrt(.5 * ((sxx - syy) ** 2 + (syy - szz) ** 2 + (szz - sxx) ** 2)
                        + 3 * (sxy ** 2 + syz ** 2 + szx ** 2))
        entry = {
            "time": time, "stroke_mm": -reaction["displacements"][1],
            "punch_force_N": -reaction["forces"][1] * factor,
            "max_nodal_averaged_mises_MPa": float(mises.max()),
            "max_nodal_averaged_peeq": max(v[0] for v in frame.get("PE", {0: (0,)}).values()),
        }
        positions = np.array([np.array(nodes[n]) + frame["DISP"][n] for n in metadata["plate_boundary"]])
        entry["max_sampled_nodal_penetration_mm"] = nodal_penetration(model, positions[:, :2], entry["stroke_mm"])
        if die_reactions is not None:
            die = die_reactions[nearest]["forces"]
            anchor = anchor_reactions[nearest]["forces"]
            entry["vertical_balance_error_N"] = abs((reaction["forces"][1] + die[1]) * factor - weight * min(time, 1))
            entry["horizontal_balance_error_N"] = abs((reaction["forces"][0] + die[0] + anchor[0]) * factor)
        history.append(entry)
    formed_time = min(frames, key=lambda value: abs(value - 1))
    if abs(formed_time - 1) > 1e-7:
        raise ValueError("Missing formed result at the end of loading")
    formed = frames[formed_time]
    result = {
        "status": "converged_research_model_not_experimentally_validated",
        "solver": "CalculiX 2.21", "units": "mm, N, s, MPa",
        "input": metadata, "converged_increments": increments,
        "history": history,
        "max_sampled_nodal_penetration_mm": max(v["max_sampled_nodal_penetration_mm"] for v in history),
        "max_vertical_balance_error_N": max((v["vertical_balance_error_N"] for v in history), default=None)
        if die_reactions is not None else None,
        "max_horizontal_balance_error_N": max((v["horizontal_balance_error_N"] for v in history), default=None)
        if die_reactions is not None else None,
        "formed_hole": hole_metrics(model, nodes, formed["DISP"]),
        "final_hole": hole_metrics(model, nodes, frames[max(frames)]["DISP"]),
        "formed_flange_angle_deg": flange_angle(model, nodes, formed["DISP"]),
        "final_flange_angle_deg": flange_angle(model, nodes, frames[max(frames)]["DISP"]),
        "limits": ["Illustrative isotropic material; no fracture/damage model.",
                   "Nodal averaged stress/plastic strain are not integration-point peaks.",
                   "Penetration check covers output boundary nodes, not full cells or continuous motion.",
                   "Mesh, contact and increment sensitivity need study comparisons.",
                   "A finished solver run alone does not validate a manufacturing process."],
    }
    if model.elastic and not model.hole_radius:
        span = model.opening + 2 * model.shoulder_radius * math.tan(math.pi / 8)
        inertia = model.width * model.thickness ** 3 / 12
        stiffness = 48 * model.young * inertia / span ** 3
        last = history[-1]
        result["beam_check"] = {"support_span_mm": span,
                                "euler_bernoulli_force_N": stiffness * model.stroke,
                                "computed_force_N": last["punch_force_N"],
                                "relative_error": abs(last["punch_force_N"] / (stiffness * model.stroke) - 1)}
    force_budget = max(.001, max(abs(v["punch_force_N"]) for v in history)*1e-4)
    result["numerical_checks"] = {
        "force_balance_budget_N": force_budget,
        "force_balance": die_reactions is not None and
        result["max_vertical_balance_error_N"] <= force_budget and
        result["max_horizontal_balance_error_N"] <= force_budget,
        "sampled_node_penetration_budget_mm": .02*model.thickness,
        "sampled_node_penetration": result["max_sampled_nodal_penetration_mm"] <= .02*model.thickness,
    }
    if model.unload:
        result["springback_angle_change_deg"] = result["formed_flange_angle_deg"]-result["final_flange_angle_deg"]
    (directory / "result.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return result, model, nodes, elements, groups, frames


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result, *_ = analyze(args.directory)
    print(json.dumps({key: value for key, value in result.items()
                      if key not in {"input", "history", "limits"}}, indent=2))
