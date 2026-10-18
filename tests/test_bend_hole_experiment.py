"""Contract checks for research input/output, without requiring the FEM solver."""

import importlib.util
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "experiments" / "bend-hole"))
from bend_hole import Model
from results import check_completion, read_frd, read_reactions


class BendHoleOutputTests(unittest.TestCase):
    def test_gui_request_requires_bounded_complete_parameters(self):
        from gui_job import validate_request
        valid = {"schema": 1, "id": "job-id", "context": "D01", "parameters":
                 {"hole": "center", "mesh": "medium", "unload": True}}
        self.assertEqual(validate_request(valid), valid["parameters"])
        import copy
        for field, value in (("schema", True), ("schema", 2), ("id", ""), ("context", None)):
            case = copy.deepcopy(valid)
            case[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_request(case)
        for field, value in (("hole", "STEP"), ("mesh", "arbitrary"), ("unload", "false")):
            case = copy.deepcopy(valid)
            case["parameters"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_request(case)
        case = copy.deepcopy(valid)
        del case["parameters"]["unload"]
        with self.assertRaises(ValueError):
            validate_request(case)

    def test_finished_banner_does_not_accept_an_incomplete_step(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "solver.log").write_text("Job finished\n")
            header = "SUMMARY\n STEP INC ATT ITRS TOTAL STEP INCREMENT\n"
            (root / "model.sta").write_text(header + "1 4 1 5 .9 .9 .1\n")
            with self.assertRaisesRegex(ValueError, "final increment"):
                check_completion(root, Model())
            (root / "model.sta").write_text(header + "1 5 1 4 1 1 .1\n")
            self.assertEqual(check_completion(root, Model()), 1)
            with self.assertRaises(ValueError):
                check_completion(root, Model(unload=True))

    def test_missing_and_nonfinite_displacements_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sample.frd"
            text = "  100CL 101 1.0 2 0 1 1\n -4  DISP 4 1\n"
            text += f" -1{1:10d}{.1:12.5E}{.2:12.5E}{.3:12.5E}\n -3\n"
            path.write_text(text)
            self.assertEqual(read_frd(path, {1})[1]["DISP"][1], (.1, .2, .3))
            with self.assertRaisesRegex(ValueError, "Incomplete displacement"):
                read_frd(path, {1, 2})
            path.write_text(text.replace(f"{.1:12.5E}", f"{float('nan'):12}"))
            with self.assertRaises(ValueError):
                read_frd(path, {1})

    def test_missing_reaction_cannot_be_replaced_by_zero(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sample.dat"
            text = " displacements (vx,vy,vz) for set PUNCH_REF and time 1.0\n\n1 0 -3 0\n"
            path.write_text(text)
            with self.assertRaisesRegex(ValueError, "Incomplete reference"):
                read_reactions(path)
            path.write_text(text + " forces (fx,fy,fz) for set PUNCH_REF and time 1.0\n\n1 0 -300 0\n")
            self.assertEqual(read_reactions(path)[1]["forces"], (0, -300, 0))

    def test_invalid_geometry_is_rejected_before_meshing(self):
        for model in (Model(hole_offset=20), Model(width=4), Model(layers=0),
                      Model(mesh_size=float("nan")), Model(stroke=0)):
            with self.subTest(model=model), self.assertRaises(ValueError):
                model.validate()
        Model().validate()

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy is in the experiment environment")
    def test_reflected_hole_has_one_closed_contour(self):
        from results import hole_metrics
        nodes = {i+1: (2*math.cos(i*math.pi/6), .5, 2*math.sin(i*math.pi/6)) for i in range(7)}
        # Floating point rotation can put this endpoint infinitesimally below the plane.
        nodes[7] = (-2, .5, -1e-16)
        result = hole_metrics(Model(half_model=True), nodes, {i: (0, 0, 0) for i in nodes})
        self.assertEqual(result["sampled_nodes"], 12)
        self.assertAlmostEqual(result["polygonal_3d_perimeter_mm"], 48*math.sin(math.pi/12))
        self.assertAlmostEqual(result["perimeter_ratio"], 1)
        self.assertLess(result["max_distance_from_fit_plane_mm"], 1e-12)

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy is in the experiment environment")
    def test_tool_void_is_not_counted_as_penetration(self):
        import numpy as np
        from results import nodal_penetration
        model = Model()
        self.assertEqual(nodal_penetration(model, np.array([[0., -1.], [20., .1]]), 0), 0)
        self.assertAlmostEqual(nodal_penetration(model, np.array([[20., -.01]]), 0), .01, places=5)
        self.assertAlmostEqual(nodal_penetration(model, np.array([[0., 1.02]]), 0), .02, places=5)

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy is in the experiment environment")
    def test_flange_angle_uses_zero_for_a_flat_plate(self):
        from results import flange_angle
        nodes, displacement = {}, {}
        for x in (-24., -22., -20., 20., 22., 24.):
            for z in (0., 5., 10.):
                n = len(nodes)+1
                nodes[n] = (x, .5, z)
                angle = math.copysign(math.pi/12, x)
                displacement[n] = (x*math.cos(angle)-.5*math.sin(angle)-x,
                                   x*math.sin(angle)+.5*math.cos(angle)-.5, 0)
        self.assertAlmostEqual(flange_angle(Model(), nodes, {n:(0,0,0) for n in nodes}), 0)
        self.assertAlmostEqual(flange_angle(Model(), nodes, displacement), 30)


if __name__ == "__main__":
    unittest.main()
