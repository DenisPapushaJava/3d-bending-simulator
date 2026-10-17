import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("version", Path(__file__).parents[1] / "scripts/version.py")
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class ReleaseVersionTests(unittest.TestCase):
    def test_supported_channels_and_stable(self):
        for value in ["2026.1.0-alpha.1", "2026.1.0-beta.2", "2026.1.0-rc.1", "2026.1.0"]:
            result = version.parse_version(value)
            self.assertEqual(result["tag"], "v" + value)
            self.assertEqual(result["numeric"], "2026.1.0")
            self.assertEqual(result["prerelease"], "-" in value)

    def test_ambiguous_or_malformed_versions_are_rejected(self):
        for value in ["v2026.1.0", "2026.1.0-alpha", "2026.1.0-alpha.0", "2026.01.0",
                      "2026.0.0", "2026.1.00", "2026.1.0-preview.1", "2026.1.0+build", "2026.1.0\n"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                version.parse_version(value)


if __name__ == "__main__":
    unittest.main()
