import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "ci_scope", Path(__file__).parents[1] / "scripts/ci_scope.py")
scope = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scope)


class CiScopeTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.git("init", "--quiet")

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root).decode().strip()

    def write(self, name, value="one"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding="utf-8")
        self.git("add", "--", name)

    def fingerprint(self):
        return scope.build_fingerprint(self.root, self.git("write-tree"))

    def test_documentation_does_not_change_build_inputs(self):
        self.write("src/main.cpp")
        before = self.fingerprint()
        for name in ["README.md", "CHANGELOG.md", "AGENTS.md", "docs/part.pdf",
                     "design/mockup.svg", "docs/вложенный/текст.txt"]:
            self.write(name)
        self.assertEqual(before, self.fingerprint())

    def test_code_configuration_dependencies_and_tests_are_inputs(self):
        for name in ["src/main.cpp", "VERSION", "CMakeLists.txt", "CMakePresets.json",
                     "vcpkg.json", "cmake/triplets/release.cmake", "LICENSE",
                     "scripts/package_windows.py", ".github/workflows/ci.yml",
                     "tests/test_ci_scope.py", "examples/example.step"]:
            with self.subTest(name=name):
                before = self.fingerprint()
                self.write(name)
                self.assertNotEqual(before, self.fingerprint())
                before = self.fingerprint()
                self.write(name, "two")
                self.assertNotEqual(before, self.fingerprint())

    def test_renames_deletions_and_file_modes_invalidate_proof(self):
        self.write("script.py")
        first = self.fingerprint()
        self.git("update-index", "--chmod=+x", "script.py")
        self.assertNotEqual(first, self.fingerprint())
        first = self.fingerprint()
        self.git("mv", "script.py", "renamed.py")
        self.assertNotEqual(first, self.fingerprint())
        first = self.fingerprint()
        self.git("rm", "-f", "renamed.py")
        self.assertNotEqual(first, self.fingerprint())

    def test_paths_are_not_split_by_spaces_or_newlines(self):
        for name in ["src/с пробелом.cpp", "src/with\nnewline.cpp"]:
            before = self.fingerprint()
            self.write(name)
            self.assertNotEqual(before, self.fingerprint())

    def test_missing_invalid_or_old_proof_requires_build(self):
        proof = self.root / "proof"
        for event in ["pull_request", "push"]:
            self.assertTrue(scope.needs_build(event, "branch", "abc", proof))
            proof.write_bytes(b"\xff")
            self.assertTrue(scope.needs_build(event, "branch", "abc", proof))
            proof.write_text("different", encoding="ascii")
            self.assertTrue(scope.needs_build(event, "branch", "abc", proof))
            proof.unlink()

    def test_only_successful_matching_proof_allows_reuse(self):
        proof = self.root / "proof"
        proof.write_text("abc\n", encoding="ascii")
        for event in ["pull_request", "push"]:
            self.assertFalse(scope.needs_build(event, "branch", "abc", proof))
        for event, ref_type in [("push", "tag"), ("workflow_dispatch", "branch"),
                                ("workflow_dispatch", "tag"), ("unknown", "branch"),
                                ("push", "")]:
            self.assertTrue(scope.needs_build(event, ref_type, "abc", proof))

    def test_workflow_outputs_force_tags_and_manual_runs(self):
        proof = self.root / "proof"
        proof.write_text(scope.build_fingerprint(), encoding="ascii")
        for event, ref_type, build, package in [
                ("pull_request", "branch", "false", "false"),
                ("push", "branch", "false", "false"),
                ("push", "tag", "true", "true"),
                ("workflow_dispatch", "branch", "true", "false")]:
            with self.subTest(event=event, ref_type=ref_type):
                output = self.root / "output"
                output.write_text("", encoding="ascii")
                environment = dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_REF_TYPE=ref_type)
                subprocess.check_output(
                    [sys.executable, str(Path(scope.__file__)), "--proof", str(proof),
                     "--github-output", str(output)], env=environment)
                values = dict(line.split("=", 1) for line in output.read_text().splitlines())
                self.assertEqual(values["build"], build)
                self.assertEqual(values["package"], package)
                self.assertEqual(len(values["fingerprint"]), 64)


if __name__ == "__main__":
    unittest.main()
