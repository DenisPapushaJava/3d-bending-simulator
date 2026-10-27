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
QT_SOURCE_SHA256 = "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"
FREETYPE_VERSION = "2.14.3"
FREETYPE_SOURCE_SHA512 = (
    "c3b6b0cc4b428c9c647ab2148386901dfd315273b68051940e8fea6010d46fdd2913467"
    "c3ef58be0d499b8e2ef5a0f1a4cc5e739756155587f4f7dff08ef9695")


def unpack_patched_source(archive_path, destination, port, patches):
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive_path) as archive:
        for member in archive.getmembers():
            relative = Path(*Path(member.name).parts[1:])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Unsafe dependency source archive path")
            if member.isfile():
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
    subprocess.run(["git", "init", "--quiet", str(destination)], check=True)
    for patch in patches:
        subprocess.run(["git", "-C", str(destination), "apply", "--ignore-whitespace",
                        str((port / patch).resolve())], check=True)
    shutil.rmtree(destination / ".git")


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
                    "--no-opengl-sw", "--no-system-d3d-compiler", "--no-system-dxc-compiler",
                    "--skip-plugin-types", "iconengines,imageformats,generic",
                    "--dir", str(stage / "bin"), str(executable)], check=True)
    inventoried_qt = {"Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll"}
    unexpected_qt = {dll.name for dll in (stage / "bin").glob("Qt6*.dll")} - inventoried_qt
    if unexpected_qt:
        raise RuntimeError(f"Update the source/license inventory before bundling {unexpected_qt}")
    # Lets the packaged startup check run headlessly; qwindows is also deployed above.
    shutil.copy2(args.qt_root / "plugins/platforms/qoffscreen.dll", stage / "bin/platforms")

    sources = output / f"3d-bending-simulator-{version}-dependency-sources"
    sources.mkdir(parents=True, exist_ok=True)
    qt_archive = sources / f"qtbase-everywhere-src-{QT_VERSION}.tar.xz"
    qt_url = f"https://download.qt.io/archive/qt/6.8/{QT_VERSION}/submodules/{qt_archive.name}"
    urllib.request.urlretrieve(qt_url, qt_archive)
    actual = hashlib.sha256(qt_archive.read_bytes()).hexdigest()
    if actual != QT_SOURCE_SHA256:
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
    shutil.copy2(args.vcpkg_root / "LICENSE.txt", sources / "vcpkg-LICENSE.txt")
    shutil.copy2(args.vcpkg_root / "LICENSE.txt", notices / "vcpkg-LICENSE.txt")
    installed = ROOT / "vcpkg_installed/x64-windows-release/share/opencascade/copyright"
    shutil.copy2(installed, notices / "OCCT-copyright.txt")
    for resource in ("Shaders", "XSMessage", "SHMessage", "XSTEPResource", "UnitsAPI"):
        resource_dir = installed.parent / "resources" / resource
        if not resource_dir.is_dir():
            raise RuntimeError(f"OCCT runtime resources are missing: {resource_dir}")
        shutil.copytree(resource_dir, stage / "share/opencascade/resources" / resource,
                        dirs_exist_ok=True)
    # A binary-cache hit does not populate vcpkg buildtrees. Reproduce sources independently.
    occt_archive = output / "OCCT-V7_9_3.tar.gz"
    urllib.request.urlretrieve("https://github.com/Open-Cascade-SAS/OCCT/archive/V7_9_3.tar.gz",
                               occt_archive)
    expected_occt = ("65935a2f46021e2b9a7dd2a218515c06925454855a8cc952fcbd1cccbfd5c8d605"
                     "ed8f1d930d2aec87ef172ded551d0a237fad128319fb9cbcabdc755aa0aa67")
    if hashlib.sha512(occt_archive.read_bytes()).hexdigest() != expected_occt:
        raise RuntimeError("OCCT sources differ from the SHA512 pinned by the vcpkg port")
    unpack_patched_source(occt_archive, sources / "occt-7.9.3-patched", port,
                          ["fix-install-prefix-path.patch", "drop-bin-letter-d.patch",
                           "dependencies.patch", "install-include-dir.patch",
                           "remove-vcpkg-enabling.patch"])
    embedded_font = (sources / "occt-7.9.3-patched/src/Font/Font_DejavuSans_Latin_woff.pxx")
    font_notice = "\n".join(line[3:].lstrip() for line in embedded_font.read_text(
        encoding="utf-8").splitlines() if line.startswith("//!"))
    if "Bitstream" not in font_notice:
        raise RuntimeError("OCCT embedded font notice is missing")
    (notices / "OCCT-DejaVu-copyright.txt").write_text(font_notice + "\n", encoding="utf-8")
    freetype_port = args.vcpkg_root / "ports/freetype"
    shutil.copytree(freetype_port, sources / "vcpkg-freetype-port", dirs_exist_ok=True)
    shutil.copy2(installed.parent.parent / "freetype/copyright",
                 notices / "FreeType-copyright.txt")
    freetype_archive = output / f"freetype-{FREETYPE_VERSION}.tar.gz"
    freetype_ref = "VER-" + FREETYPE_VERSION.replace(".", "-")
    urllib.request.urlretrieve(
        "https://gitlab.freedesktop.org/freetype/freetype/-/archive/"
        f"{freetype_ref}/freetype-{freetype_ref}.tar.gz", freetype_archive)
    if hashlib.sha512(freetype_archive.read_bytes()).hexdigest() != FREETYPE_SOURCE_SHA512:
        raise RuntimeError("FreeType sources differ from the SHA512 pinned by the vcpkg port")
    unpack_patched_source(freetype_archive, sources / f"freetype-{FREETYPE_VERSION}-patched",
                          freetype_port, ["0003-Fix-UWP.patch", "brotli-static.patch",
                                          "bzip2.patch", "fix-exports.patch"])
    # These bundled source files retain their own MIT/X11/zlib notices beyond FTL.
    for relative in ["src/bdf/README", "src/pcf/README", "src/gzip/zlib.h",
                     "src/autofit/ft-hb-ft.c", "src/autofit/ft-hb-decls.h",
                     "src/autofit/ft-hb-types.h", "src/autofit/hb-script-list.h",
                     "src/base/fthash.c", "include/freetype/internal/fthash.h"]:
        target = notices / "freetype" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(sources / f"freetype-{FREETYPE_VERSION}-patched" / relative, target)
    for path in ["vcpkg.json", "CMakePresets.json", "THIRD_PARTY_NOTICES.md", "LICENSE"]:
        shutil.copy2(ROOT / path, sources / path)
    shutil.copytree(ROOT / "cmake/triplets", sources / "triplets", dirs_exist_ok=True)
    (sources / "README.txt").write_text(
        "OCCT and FreeType sources include vcpkg port patches.\n"
        "The port files, manifest and triplet describe the build.\n"
        "FreeType patches: 0003-Fix-UWP, brotli-static, bzip2, fix-exports.\n"
        "Optional FreeType features are disabled; the FTL license option is selected.\n"
        "vcpkg baseline: 256310531c1e57ee83f8ac01f7bea7c1e56630f5.\n"
        "Qt base sources include bundled third-party source/license materials.\n"
        "See https://github.com/DenisPapushaJava/3d-bending-simulator/blob/main/docs/development.md\n",
        encoding="utf-8")
    (stage / "README.txt").write_text(
        f"3D Bending Simulator {version} — экспериментальный выпуск Windows x64.\n\n"
        "Распакуйте весь архив и запустите bin/3d-bending-simulator.exe.\n"
        "Откройте STEP/STP одной детали или проект .bend; файл можно перетащить в окно.\n"
        "Этапы работы: Деталь → Инструмент → Проверка.\n"
        "Учебные STEP: share/3d-bending-simulator/examples.\n\n"
        "Доступны просмотр детали, распознавание поддержанных прямых сгибов,\n"
        "подбор по параметрам, геометрический просмотр одного сгиба и справочный PDF.\n"
        "Это не физический расчёт материала и не проверка полного маршрута гибки.\n"
        "Интерактивная работа на Windows/GPU ещё требует проверки.\n\n"
        "Может потребоваться Microsoft Visual C++ 2015–2022 x64 Runtime:\n"
        "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\n"
        "Сохраняйте структуру папок, DLL и плагины рядом с EXE.\n"
        "DLL допускается заменять совместимыми сборками. Лицензии — в share/licenses.\n"
        "Соответствующие исходники Qt/OCCT/FreeType — в соседнем dependency-sources ZIP.\n"
        "Встроенное обновление пока отсутствует.\n"
        "Проект: https://github.com/DenisPapushaJava/3d-bending-simulator\n",
        encoding="utf-8")

    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment.pop("QT_PLUGIN_PATH", None)
    environment.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)
    for variable in list(environment):
        if variable.startswith("CSF_"):
            environment.pop(variable)
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    font_probe = stage / "bin/_font-probe.exe"
    shutil.copy2(ROOT / "build/release/bin/viewer_font_tests.exe", font_probe)
    try:
        subprocess.run([str(font_probe)], env=environment, check=True, timeout=20)
    finally:
        font_probe.unlink()
    subprocess.run([str(executable), "--smoke-test"], env=environment, check=True, timeout=20)
    subprocess.run([str(executable), "--smoke-test",
                    str(stage / "share/3d-bending-simulator/examples/plate-inch.stp")],
                   env=environment, check=True, timeout=30)
    archive_tree(output / f"{name}.zip", stage)
    archive_tree(output / f"{sources.name}.zip", sources)


if __name__ == "__main__":
    main()
