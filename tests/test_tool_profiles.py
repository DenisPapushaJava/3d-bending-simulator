"""Synthetic, redistributable contours; no workshop files are checked in."""
import importlib.util
import math
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('tool_profiles', Path(__file__).resolve().parents[1] / 'tools/import_tool_profiles.py')
PROFILES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILES)


class ProfileImportTests(unittest.TestCase):
    def test_order_and_reverse_arc(self):
        edges = [PROFILES.segment([1, 0], [-1, 0], -1),
                 PROFILES.segment([1, 0], [-1, 0])]
        result, ignored = PROFILES.ordered_contour(edges)
        self.assertEqual(ignored, 0)
        self.assertEqual(len(result), 2)
        self.assertAlmostEqual(abs(result[0]['sweepRadians']), math.pi)
        self.assertEqual(result[0]['end'], result[1]['start'])
        self.assertEqual(result[1]['end'], result[0]['start'])

    def test_reject_ambiguous_and_open(self):
        def triangle(x):
            return [PROFILES.segment([x, 0], [x + 1, 0]),
                    PROFILES.segment([x + 1, 0], [x, 1]),
                    PROFILES.segment([x, 1], [x, 0])]
        with self.assertRaises(ValueError):
            PROFILES.ordered_contour(triangle(0) + triangle(10))
        with self.assertRaises(ValueError):
            PROFILES.ordered_contour(triangle(0)[:2])

    def test_units_and_unsupported_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ST1.dxf'
            def write(typ):
                path.write_text('0\nSECTION\n2\nHEADER\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n' + typ + '0\nENDSEC\n0\nEOF\n')
            write('0\nLINE\n10\n0\n20\n0\n11\n1\n21\n0\n0\nLINE\n10\n1\n20\n0\n11\n0\n21\n1\n0\nLINE\n10\n0\n20\n1\n11\n0\n21\n0\n')
            catalog = {'tools': [{'code': 'ST1S'}]}
            report = PROFILES.import_profiles(catalog, Path(directory))
            self.assertFalse(catalog['tools'][0]['drawing']['millimeters'])
            self.assertEqual(report[0]['edges'], 3)
            self.assertEqual(len(catalog['tools'][0]['drawing']['sha256']), 64)
            PROFILES.import_profiles(catalog, Path(directory), {'ST1'})
            self.assertTrue(catalog['tools'][0]['drawing']['millimeters'])
            write('0\nINSERT\n2\nunknown-block\n')
            with self.assertRaises(ValueError):
                PROFILES.parse_dxf(path)


if __name__ == '__main__':
    unittest.main()
