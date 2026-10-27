import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("windows_sdk", Path(__file__).parents[1] / "scripts/windows_sdk.py")
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class WindowsSdkTests(unittest.TestCase):
    def fixture(self, root, *, path=None, package_hash=None, compiler="14.44"):
        data = b"binary package fixture"
        abi = "ab" * 32
        relative = sdk.cache_path(abi)
        metadata = {"schema": 1, "input_sha256": "inputs", "compiler": compiler,
                    "packages": [{"abi": abi, "path": path or relative,
                                  "sha256": package_hash or hashlib.sha256(data).hexdigest()}]}
        archive = root / "sdk.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("sdk.json", json.dumps(metadata))
            z.writestr("packages/" + relative, data)
        return archive, relative, data

    def test_restore_binary_package_without_extracting_other_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, relative, data = self.fixture(root)
            sdk.restore_archive(archive, root / "cache", sdk.sha256(archive), "inputs", "14.44")
            self.assertEqual((root / "cache" / relative).read_bytes(), data)
            self.assertFalse((root / "cache/sdk.json").exists())

    def test_archive_corruption_does_not_write_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive, _, _ = self.fixture(root)
            with self.assertRaisesRegex(ValueError, "archive checksum"):
                sdk.restore_archive(archive, root / "cache", "bad", "inputs", "14.44")
            self.assertFalse((root / "cache").exists())

    def test_bad_package_hash_or_path_or_compiler_is_rejected_before_write(self):
        for options in [{"package_hash": "bad"}, {"path": "../escape.zip"}, {"compiler": "other"}]:
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                archive, _, _ = self.fixture(root, **options)
                with self.assertRaises(ValueError):
                    sdk.restore_archive(archive, root / "cache", sdk.sha256(archive), "inputs", "14.44")
                self.assertFalse((root / "cache").exists())

    def test_status_skips_feature_and_uninstalled_entries(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "vcpkg").mkdir()
            paragraphs = [f"Package: {name}\nVersion: 1.0\nArchitecture: x64-windows-release\n"
                          f"Abi: {letter * 64}\nStatus: install ok installed"
                          for name, letter in [("opencascade", "a"), ("freetype", "b")]]
            paragraphs += ["Package: opencascade\nFeature: freetype\nStatus: install ok installed",
                           "Package: old\nStatus: deinstall ok not-installed"]
            (root / "vcpkg/status").write_text("\n\n".join(paragraphs))
            self.assertEqual([p["package"] for p in sdk.installed_packages(root)], ["freetype", "opencascade"])

    def test_different_inputs_skip_snapshot_without_network(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "cmake/triplets").mkdir(parents=True)
            (root / "vcpkg.json").write_text("{}")
            lock = root / "lock.json"
            lock.write_text(json.dumps({"schema": 1, "input_sha256": "other", "compiler": "14.44"}))
            sdk.restore(root, lock, root / "cache", "14.44")
            self.assertFalse((root / "cache").exists())


if __name__ == "__main__":
    unittest.main()
