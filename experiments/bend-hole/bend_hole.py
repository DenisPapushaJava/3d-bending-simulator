#!/usr/bin/env python3
"""Reproducible research model; millimetres, newtons, seconds and MPa. MIT."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import subprocess
import time


@dataclass(frozen=True)
class Model:
    length: float = 60.0
    width: float = 20.0
    thickness: float = 1.0
    hole_radius: float = 2.0
    hole_offset: float = 0.0
    opening: float = 24.0
    shoulder_radius: float = 2.0
    nose_radius: float = 2.0
    punch_angle: float = 85.0
    stroke: float = 3.0
    mesh_size: float = 2.0
    hole_size: float = 0.8
    layers: int = 2
    young: float = 210000.0
    poisson: float = 0.3
    friction: float = 0.0
    penalty: float = 100000.0
    elastic: bool = False
    unload: bool = False
    half_model: bool = False
    max_increment: float = 0.025

    def validate(self):
        numeric = [value for value in asdict(self).values() if not isinstance(value, bool)]
        if not all(math.isfinite(value) for value in numeric):
            raise ValueError("Model parameters must be finite")
        if not (40 <= self.length <= 70 and 4 <= self.width <= 30 and .5 <= self.thickness <= 2):
            raise ValueError("Outside the research model size range")
        if not (.25 <= self.mesh_size <= 4 and .15 <= self.hole_size <= self.mesh_size and 1 <= self.layers <= 6):
            raise ValueError("Outside the mesh budget")
        if self.hole_radius < 0 or self.hole_radius + abs(self.hole_offset) >= self.opening / 2 - 2:
            raise ValueError("The hole must be inside the V span")
        if self.hole_radius and self.hole_radius + self.thickness >= self.width / 2:
            raise ValueError("Not enough material beside the hole")
        if not (.001 <= self.stroke <= 6 and 0 <= self.friction <= .5 and .001 <= self.max_increment <= .1):
            raise ValueError("Outside the process range")
        if not (self.young > 0 and 0 <= self.poisson < .49 and 1e3 <= self.penalty <= 1e6):
            raise ValueError("Invalid material or contact parameters")


# Abaqus/CalculiX volume-face numbering; the exporter supplies Abaqus connectivity.
FACES = {
    "C3D20": [
        (0, 1, 2, 3, 8, 9, 10, 11),
        (4, 5, 6, 7, 12, 13, 14, 15),
        (0, 1, 5, 4, 8, 17, 12, 16),
        (1, 2, 6, 5, 9, 18, 13, 17),
        (2, 3, 7, 6, 10, 19, 14, 18),
        (3, 0, 4, 7, 11, 16, 15, 19),
    ],
    "C3D15": [
        (0, 1, 2, 6, 7, 8),
        (3, 4, 5, 9, 10, 11),
        (0, 1, 4, 3, 6, 13, 9, 12),
        (1, 2, 5, 4, 7, 14, 10, 13),
        (2, 0, 3, 5, 8, 12, 11, 14),
    ],
}


def parse_mesh(path):
    nodes, elements, groups = {}, {}, {}
    mode, kind, group = "", "", ""
    pending = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("**"):
            continue
        if line.startswith("*"):
            if pending:
                raise ValueError("Truncated element connectivity")
            parts = line.upper().replace(" ", "").split(",")
            mode = parts[0]
            options = dict(part.split("=", 1) for part in parts[1:] if "=" in part)
            if mode == "*ELEMENT":
                kind = options["TYPE"]
                if kind not in FACES:
                    raise ValueError(f"Unsupported exported element {kind}")
            elif mode == "*ELSET":
                group = options["ELSET"]
                groups.setdefault(group, [])
            continue
        fields = [v.strip() for v in line.split(",") if v.strip()]
        if mode == "*NODE":
            nodes[int(fields[0])] = tuple(map(float, fields[1:4]))
        elif mode == "*ELEMENT":
            pending.extend(map(int, fields))
            count = 20 if kind == "C3D20" else 15
            if len(pending) == count + 1:
                elements[pending[0]] = (kind, tuple(pending[1:]))
                pending = []
            elif len(pending) > count + 1:
                raise ValueError("Invalid element connectivity")
        elif mode == "*ELSET":
            groups[group].extend(map(int, fields))
    if not nodes or any(not groups.get(name) for name in ("PLATE", "PUNCH", "DIE")):
        raise ValueError("Missing model mesh groups")
    return nodes, elements, groups


def boundary(elements, ids):
    occurrences = Counter()
    records = []
    for eid in ids:
        kind, connectivity = elements[eid]
        for side, indices in enumerate(FACES[kind], 1):
            ids_on_face = tuple(connectivity[i] for i in indices)
            key = tuple(sorted(ids_on_face))
            occurrences[key] += 1
            records.append((key, eid, side, ids_on_face))
    return [(eid, side, face) for key, eid, side, face in records if occurrences[key] == 1]


def mesh(model, directory):
    import gmsh

    gmsh.initialize(["bend-hole", "-nopopup"])
    try:
        gmsh.option.setNumber("General.Terminal", 1)
        gmsh.option.setNumber("General.NumThreads", 1)
        gmsh.option.setNumber("Mesh.Algorithm", 6)
        gmsh.option.setNumber("Mesh.RecombineAll", 1)
        gmsh.option.setNumber("Mesh.MeshSizeMin", min(model.hole_size, model.mesh_size))
        gmsh.option.setNumber("Mesh.MeshSizeMax", model.mesh_size)
        gmsh.option.setNumber("Mesh.MeshSizeFromCurvature", 24)
        gmsh.option.setNumber("Mesh.SecondOrderIncomplete", 1)
        gmsh.model.add("bend-hole")
        occ = gmsh.model.occ
        plate_width = model.width / 2 if model.half_model else model.width
        plate = occ.addRectangle(-model.length / 2, 0 if model.half_model else -model.width / 2,
                                 0, model.length, plate_width)
        if model.hole_radius:
            hole = occ.addDisk(model.hole_offset, 0, 0, model.hole_radius, model.hole_radius)
            plate = occ.cut([(2, plate)], [(2, hole)])[0][0][1]
        anchor_point = occ.addPoint(0, model.width / 2, 0)
        divided = occ.fragment([(2, plate)], [(0, anchor_point)])[0]
        plate = next(tag for dim, tag in divided if dim == 2)
        occ.rotate([(2, plate)], 0, 0, 0, 1, 0, 0, math.pi / 2)
        extruded = occ.extrude([(2, plate)], 0, model.thickness, 0,
                               numElements=[model.layers], recombine=True)
        plate_volume = next(tag for dim, tag in extruded if dim == 3)

        def profile(points, arcs, name):
            # A distinct body: coincident contact nodes must not be merged.
            z = -2 if model.half_model else -model.width / 2 - 2
            tags = [occ.addPoint(x, y, z) for x, y in points]
            lines = []
            for i in range(len(tags)):
                j = (i + 1) % len(tags)
                if i in arcs:
                    x, y = arcs[i]
                    center = occ.addPoint(x, y, z)
                    lines.append(occ.addCircleArc(tags[i], center, tags[j]))
                else:
                    lines.append(occ.addLine(tags[i], tags[j]))
            surface = occ.addPlaneSurface([occ.addCurveLoop(lines)])
            result = occ.extrude([(2, surface)], 0, 0, plate_width + 4,
                                  numElements=[1], recombine=True)
            return next(tag for dim, tag in result if dim == 3)

        beta = math.radians(model.punch_angle / 2)
        r = model.nose_radius
        tip = model.thickness
        cx = r * math.cos(beta)
        cy = tip + r
        tangent_y = cy - r * math.sin(beta)
        top_y = tip + 12
        top_x = cx + (top_y - tangent_y) * math.tan(beta)
        punch_volume = profile(
            [(-top_x, top_y), (-cx, tangent_y), (0, tip), (cx, tangent_y), (top_x, top_y)],
            {1: (0, cy), 2: (0, cy)}, "PUNCH")
        a, r = model.opening / 2, model.shoulder_radius
        d = r * math.tan(math.pi / 8)
        q = d / math.sqrt(2)
        die_volume = profile(
            [(-35, 0), (-a - d, 0), (-a + q, -q), (0, -a),
             (a - q, -q), (a + d, 0), (35, 0), (35, -20), (-35, -20)],
            {1: (-a - d, -r), 4: (a + d, -r)}, "DIE")
        occ.synchronize()
        for name, volume in [("PLATE", plate_volume), ("PUNCH", punch_volume), ("DIE", die_volume)]:
            tag = gmsh.model.addPhysicalGroup(3, [volume])
            gmsh.model.setPhysicalName(3, tag, name)
        gmsh.model.mesh.generate(3)
        gmsh.model.mesh.setOrder(2)
        plate_tags = [int(tag) for tags in gmsh.model.mesh.getElements(3, plate_volume)[1] for tag in tags]
        minimum_jacobian = min(gmsh.model.mesh.getElementQualities(plate_tags, "minDetJac"))
        if not math.isfinite(minimum_jacobian) or minimum_jacobian <= 0:
            raise ValueError("Invalid plate element Jacobian")
        expected_volume = (model.length * model.width - math.pi * model.hole_radius ** 2) * model.thickness
        if model.half_model:
            expected_volume /= 2
        volume = occ.getMass(3, plate_volume)
        if abs(volume / expected_volume - 1) > 1e-8:
            raise ValueError("Plate CAD volume does not match the analytic volume")
        mesh_check = {"gmsh_version": gmsh.__version__, "analytic_volume_mm3": expected_volume,
                      "cad_volume_mm3": volume, "sampled_minimum_element_jacobian": float(minimum_jacobian)}
        (directory / "mesh-check.json").write_text(json.dumps(mesh_check, indent=2) + "\n", encoding="utf-8")
        gmsh.write(str(directory / "mesh.inp"))
        gmsh.write(str(directory / "mesh.msh"))
    finally:
        gmsh.finalize()
    return parse_mesh(directory / "mesh.inp")


def write_deck(model, directory):
    nodes, elements, groups = mesh(model, directory)
    surfaces = {name: boundary(elements, ids) for name, ids in groups.items()}
    plate_nodes = sorted({n for eid in groups["PLATE"] for n in elements[eid][1]})
    plate_boundary = sorted({n for _, _, face in surfaces["PLATE"] for n in face})
    refs = {}
    for name in ("PUNCH", "DIE"):
        reference, rotation = max(nodes) + 1, max(nodes) + 2
        nodes[reference] = (0, 0, 0)
        nodes[rotation] = (0, 0, 0)
        refs[name] = reference, rotation
    lines = ["*HEADING", "Research only: illustrative isotropic material, mm N s MPa", "*NODE"]
    lines.extend(f"{n}, {x:.12g}, {y:.12g}, {z:.12g}" for n, (x, y, z) in nodes.items())
    for kind in FACES:
        output_kind = "C3D20R" if kind == "C3D20" else kind
        lines.append(f"*ELEMENT,TYPE={output_kind}")
        for eid, (element_kind, connectivity) in elements.items():
            if element_kind == kind:
                fields = [eid, *connectivity]
                for i in range(0, len(fields), 16):
                    lines.append(", ".join(map(str, fields[i:i + 16])))

    def add_set(keyword, name, ids):
        lines.append(f"*{keyword},{keyword}={name}")
        for i in range(0, len(ids), 12):
            lines.append(", ".join(map(str, ids[i:i + 12])))

    for name, ids in groups.items():
        add_set("ELSET", name, ids)
        add_set("NSET", name, sorted({n for eid in ids for n in elements[eid][1]}))
    add_set("NSET", "PLATE_BOUNDARY", plate_boundary)
    add_set("NSET", "WIDTH_SYMMETRY", [n for n in plate_nodes if abs(nodes[n][2]) < 1e-8])
    anchor = min(plate_nodes, key=lambda n: nodes[n][0] ** 2 + (nodes[n][2] - model.width / 2) ** 2
                 + (nodes[n][1] - model.thickness / 2) ** 2)
    if sum((a - b) ** 2 for a, b in zip(nodes[anchor], (0, model.thickness / 2, model.width / 2))) > 1e-12:
        raise ValueError("The midpoint restraint node is missing")
    add_set("NSET", "ANCHOR", [anchor])
    for name, (reference, rotation) in refs.items():
        add_set("NSET", f"{name}_REF", [reference])
        lines.append(f"*RIGID BODY,NSET={name},REF NODE={reference},ROT NODE={rotation}")
    lines += ["*MATERIAL,NAME=SHEET", "*ELASTIC", f"{model.young}, {model.poisson}",
              "*DENSITY", "7.85e-9"]
    if not model.elastic:
        lines += ["*PLASTIC", "250, 0", "350, 0.02", "450, 0.1", "550, 0.25"]
    lines += ["*MATERIAL,NAME=TOOL", "*ELASTIC", "210000, 0.3", "*DENSITY", "7.85e-9"]
    for name in groups:
        lines += [f"*SOLID SECTION,ELSET={name},MATERIAL={'SHEET' if name == 'PLATE' else 'TOOL'}"]
    lines.append("*SURFACE,NAME=SHEET_CONTACT,TYPE=ELEMENT")
    lines.extend(f"{eid}, S{side}" for eid, side, face in surfaces["PLATE"]
                 if not (model.half_model and all(abs(nodes[n][2]) < 1e-8 for n in face)))
    for name in ("PUNCH", "DIE"):
        lines.append(f"*SURFACE,NAME={name}_CONTACT,TYPE=ELEMENT")
        lines.extend(f"{eid}, S{side}" for eid, side, _ in surfaces[name])
    lines += ["*SURFACE INTERACTION,NAME=CONTACT", "*SURFACE BEHAVIOR,PRESSURE-OVERCLOSURE=LINEAR",
              f"{model.penalty}, 0.01, 0.001"]
    if model.friction > 0:
        lines += ["*FRICTION", f"{model.friction}, 10000"]
    lines += ["*CONTACT PAIR,INTERACTION=CONTACT,TYPE=SURFACE TO SURFACE",
              "SHEET_CONTACT,PUNCH_CONTACT", "SHEET_CONTACT,DIE_CONTACT", "*BOUNDARY",
              "WIDTH_SYMMETRY,3,3", "ANCHOR,1,1"]
    for name, (reference, rotation) in refs.items():
        lines.append(f"{rotation},1,3")
        if name == "DIE":
            lines.append(f"{reference},1,3")
        else:
            lines += [f"{reference},1,1", f"{reference},3,3"]
    for step, stroke in enumerate([model.stroke, 0.0] if model.unload else [model.stroke], 1):
        lines += ["*STEP,NLGEOM,INC=2000", "*STATIC", f"0.01, 1.0, 1e-6, {model.max_increment}",
                  "*BOUNDARY", f"{refs['PUNCH'][0]},2,2,{-stroke}", "*DLOAD",
                  "PLATE,GRAV,9810,0,-1,0", "*NODE FILE,FREQUENCY=1", "U,RF",
                  "*EL FILE,FREQUENCY=1", "S" if model.elastic else "S,PEEQ",
                  "*NODE PRINT,NSET=PUNCH_REF,FREQUENCY=1", "U,RF",
                  "*NODE PRINT,NSET=DIE_REF,FREQUENCY=1", "RF",
                  "*NODE PRINT,NSET=ANCHOR,FREQUENCY=1", "RF", "*END STEP"]
    path = directory / "model.inp"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    metadata = {"model": asdict(model), "nodes": len(nodes), "elements": len(elements),
                "plate_elements": len(groups["PLATE"]), "plate_nodes": plate_nodes,
                "plate_boundary": plate_boundary, "references": refs,
                "anchor_node": anchor, "anchor_position_mm": nodes[anchor],
                "deck_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    (directory / "input.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--stroke", type=float, default=3)
    parser.add_argument("--mesh-size", type=float, default=2)
    parser.add_argument("--hole-size", type=float, default=.8)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--width", type=float, default=20)
    parser.add_argument("--friction", type=float, default=0)
    parser.add_argument("--penalty", type=float, default=100000)
    parser.add_argument("--hole-offset", type=float, default=0)
    parser.add_argument("--no-hole", action="store_true")
    parser.add_argument("--elastic", action="store_true")
    parser.add_argument("--unload", action="store_true")
    parser.add_argument("--half-model", action="store_true")
    parser.add_argument("--max-increment", type=float, default=.025)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output directory already exists; keep previous experiments reproducible")
    model = Model(stroke=args.stroke, mesh_size=args.mesh_size, hole_size=args.hole_size,
                  layers=args.layers, width=args.width, hole_offset=args.hole_offset,
                  hole_radius=0 if args.no_hole else 2, friction=args.friction, penalty=args.penalty,
                  elastic=args.elastic, unload=args.unload, half_model=args.half_model,
                  max_increment=args.max_increment)
    try:
        model.validate()
    except ValueError as exc:
        parser.error(str(exc))
    args.output.mkdir(parents=True)
    metadata = write_deck(model, args.output)
    print(json.dumps({key: metadata[key] for key in ("nodes", "elements", "plate_elements")}), flush=True)
    if not args.generate_only:
        start = time.monotonic()
        env = {**os.environ, "OMP_NUM_THREADS": "2", "CCX_NPROC_RESULTS": "2"}
        run = {"status": "running", "python": platform.python_version(),
               "platform": platform.platform(), "threads": 2,
               "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
        try:
            with (args.output / "solver.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(["ccx", "-i", "model"], cwd=args.output, env=env,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        except subprocess.TimeoutExpired:
            run.update(status="timeout", seconds=time.monotonic() - start)
            (args.output / "run.json").write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
            raise SystemExit("Solver time budget exhausted; no completed result")
        run.update(status="completed" if result.returncode == 0 else "failed",
                   exit_code=result.returncode, seconds=time.monotonic() - start)
        (args.output / "run.json").write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
        print(f"Solver exit={result.returncode}, seconds={time.monotonic() - start:.2f}", flush=True)
        if result.returncode or "Job finished" not in (args.output / "solver.log").read_text(encoding="utf-8"):
            raise SystemExit("Solver failed; inspect solver.log and model.sta")
        from results import analyze
        analyze(args.output)


if __name__ == "__main__":
    main()
