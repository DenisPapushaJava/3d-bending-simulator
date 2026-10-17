#!/usr/bin/env python3
"""Assemble the Windows ZIP and its corresponding dependency sources."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
QT_VERSION = "6.8.3"


def archive_tree(output, folder):
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(folder.parent))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", type=Path, required=True)
    parser.add_argument("--vcpkg-root", type=Path, required=True)
    args = parser.parse_args()
    version = (ROOT / "VERSION").read_text().strip()
    output = ROOT / "out"
    name = f"3d-bending-simulator-{version}-windows-x86_64"
    stage = output / name
    stage.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmake", "--install", "build/release", "--prefix", str(stage)], check=True)
    for dll in (ROOT / "build/release/bin").glob("*.dll"):
        shutil.copy2(dll, stage / "bin" / dll.name)
    deploy = args.qt_root / "bin/windeployqt.exe"
    executable = stage / "bin/3d-bending-simulator.exe"
    subprocess.run([str(deploy), "--release", "--no-translations", "--no-compiler-runtime",
                    "--dir", str(stage / "bin"), str(executable)], check=True)
    # Lets the packaged startup check run headlessly; qwindows is also deployed above.
    shutil.copy2(args.qt_root / "plugins/platforms/qoffscreen.dll", stage / "bin/platforms")

    sources = output / f"3d-bending-simulator-{version}-dependency-sources"
    sources.mkdir(parents=True, exist_ok=True)
    qt_archive = sources / f"qtbase-everywhere-src-{QT_VERSION}.tar.xz"
    qt_url = f"https://download.qt.io/archive/qt/6.8/{QT_VERSION}/submodules/{qt_archive.name}"
    urllib.request.urlretrieve(qt_url, qt_archive)
    with urllib.request.urlopen(qt_url + ".sha256") as response:
        expected = response.read().decode().split()[0]
    actual = hashlib.sha256(qt_archive.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError("Qt source archive checksum mismatch")

    notices = stage / "share/licenses"
    notices.mkdir(parents=True, exist_ok=True)
    with tarfile.open(qt_archive) as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            is_notice = "/LICENSES/" in member.name or (
                "/src/3rdparty/" in member.name and
                any(word in path.name.lower() for word in ["license", "copying", "copyright"]))
            if member.isfile() and is_notice:
                # Construct a safe relative path; do not extract archive paths blindly.
                relative = Path(*path.parts[1:])
                if relative.is_absolute() or ".." in relative.parts:
                    raise RuntimeError("Unsafe source archive path")
                target = notices / "qtbase" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as destination:
                    shutil.copyfileobj(source, destination)

    port = args.vcpkg_root / "ports/opencascade"
    shutil.copytree(port, sources / "vcpkg-port", dirs_exist_ok=True)
    installed = ROOT / "vcpkg_installed/x64-windows-release/share/opencascade/copyright"
    shutil.copy2(installed, notices / "OCCT-copyright.txt")
    # A binary-cache hit does not populate vcpkg buildtrees. Reproduce sources independently.
    occt_archive = output / "OCCT-V7_9_3.tar.gz"
    urllib.request.urlretrieve("https://github.com/Open-Cascade-SAS/OCCT/archive/V7_9_3.tar.gz",
                               occt_archive)
    expected_occt = ("65935a2f46021e2b9a7dd2a218515c06925454855a8cc952fcbd1cccbfd5c8d605"
                     "ed8f1d930d2aec87ef172ded551d0a237fad128319fb9cbcabdc755aa0aa67")
    if hashlib.sha512(occt_archive.read_bytes()).hexdigest() != expected_occt:
        raise RuntimeError("OCCT sources differ from the SHA512 pinned by the vcpkg port")
    occt_sources = sources / "occt-7.9.3-patched"
    occt_sources.mkdir(parents=True, exist_ok=True)
    with tarfile.open(occt_archive) as archive:
        for member in archive.getmembers():
            relative = Path(*Path(member.name).parts[1:])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Unsafe OCCT archive path")
            if member.isfile():
                target = occt_sources / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
    subprocess.run(["git", "init", "--quiet", str(occt_sources)], check=True)
    for patch in ["fix-install-prefix-path.patch", "drop-bin-letter-d.patch", "dependencies.patch",
                  "install-include-dir.patch", "remove-vcpkg-enabling.patch"]:
        subprocess.run(["git", "-C", str(occt_sources), "apply", "--ignore-whitespace",
                        str((port / patch).resolve())], check=True)
    shutil.rmtree(occt_sources / ".git")
    for path in ["vcpkg.json", "CMakePresets.json", "THIRD_PARTY_NOTICES.md"]:
        shutil.copy2(ROOT / path, sources / path)
    shutil.copytree(ROOT / "cmake/triplets", sources / "triplets", dirs_exist_ok=True)
    (sources / "README.txt").write_text(
        "OCCT sources include vcpkg port patches. The port files and triplet describe the build.\n"
        "vcpkg baseline: 256310531c1e57ee83f8ac01f7bea7c1e56630f5.\n"
        "Qt base sources include bundled third-party source/license materials.\n"
        "See https://github.com/DenisPapushaJava/3d-bending-simulator/blob/main/docs/development.md\n",
        encoding="utf-8")
    (stage / "README.txt").write_text(
        "Run bin/3d-bending-simulator.exe. This is the foundation, not a working bending simulator.\n"
        "Microsoft Visual C++ 2015-2022 x64 Runtime may be required:\n"
        "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\n"
        "Keep DLLs and plugins with the executable. They may be replaced with compatible builds.\n"
        "Matching dependency sources are in the companion dependency-sources ZIP.\n",
        encoding="utf-8")

    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment.pop("QT_PLUGIN_PATH", None)
    environment.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    subprocess.run([str(executable), "--smoke-test"], env=environment, check=True, timeout=20)
    archive_tree(output / f"{name}.zip", stage)
    archive_tree(output / f"{sources.name}.zip", sources)


if __name__ == "__main__":
    main()
