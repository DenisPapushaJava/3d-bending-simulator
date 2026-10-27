#!/usr/bin/env python3
"""Preserve and restore ABI-keyed vcpkg packages; never install an SDK directly."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RELEASES = "https://github.com/DenisPapushaJava/3d-bending-simulator/releases/download/"
SOURCES = {
    "url": RELEASES + "v2026.1.0-alpha.1/3d-bending-simulator-2026.1.0-alpha.1-dependency-sources.zip",
    "sha256": "69971ab7a34582157d322092ec98c0611da5b94d8bfdfb3f916e0c4a83abfa92",
}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def input_hash(root):
    # Same order as hashFiles('vcpkg.json', 'cmake/triplets/*.cmake').
    files = [root / "vcpkg.json", *sorted((root / "cmake/triplets").glob("*.cmake"))]
    return hashlib.sha256(b"".join(bytes.fromhex(sha256(p)) for p in files)).hexdigest()


def cache_path(abi):
    if not isinstance(abi, str) or not re.fullmatch(r"[0-9a-f]{64}", abi):
        raise ValueError("Invalid vcpkg package ABI")
    return f"{abi[:2]}/{abi}.zip"


def installed_packages(installed):
    records = []
    status = (installed / "vcpkg/status").read_text(encoding="utf-8")
    for paragraph in status.strip().split("\n\n"):
        fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if ": " in line)
        if "Feature" in fields or fields.get("Status") != "install ok installed":
            continue
        record = {key.lower(): fields[key] for key in ("Package", "Version", "Architecture", "Abi")}
        record["path"] = cache_path(record["abi"])
        records.append(record)
    if not {"opencascade", "freetype"} <= {p["package"] for p in records}:
        raise ValueError("Installed OCCT and FreeType are required")
    paths = [p["path"] for p in records]
    if len(paths) != len(set(paths)):
        raise ValueError("Duplicate installed package ABI")
    return sorted(records, key=lambda p: p["package"])


def pack(root, cache, installed, output):
    packages = installed_packages(installed)
    for record in packages:
        path = cache / record["path"]
        with zipfile.ZipFile(path) as archive:
            if "CONTROL" not in archive.namelist() or archive.testzip() is not None:
                raise ValueError(f"Invalid binary package: {path}")
        record["sha256"] = sha256(path)
    metadata = {
        "schema": 1,
        "input_sha256": input_hash(root),
        "baseline": json.loads((root / "vcpkg.json").read_text())["builtin-baseline"],
        "compiler": os.environ["VCToolsVersion"].rstrip("/\\"),
        "runner_image": os.environ.get("ImageVersion", "unknown"),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "packages": packages,
        "sources": SOURCES,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("sdk.json", json.dumps(metadata, indent=2) + "\n")
        archive.writestr("README.txt", "Windows x64 Release SDK binary cache. Not an application.\n"
                         "Import packages through vcpkg: ABI/compiler checks must remain enabled.\n"
                         "Package ZIPs retain their upstream copyright/license files.\n"
                         "Corresponding OCCT/FreeType sources and patched build recipes: see sdk.json.\n")
        archive.write(root / "vcpkg.json", "vcpkg.json")
        archive.write(root / "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md")
        for path in sorted((root / "cmake/triplets").glob("*.cmake")):
            archive.write(path, "cmake/triplets/" + path.name)
        for record in packages:
            archive.write(cache / record["path"], "packages/" + record["path"])
    digest = sha256(output)
    output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    output.with_name("SHA256SUMS.txt").write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(f"Preserved {len(packages)} installed packages: {output.name}, SHA256 {digest}")


def restore_archive(archive_path, cache, expected_hash, expected_inputs, compiler):
    if sha256(archive_path) != expected_hash:
        raise ValueError("SDK archive checksum mismatch")
    with zipfile.ZipFile(archive_path) as archive:
        metadata = json.loads(archive.read("sdk.json"))
        if (metadata["schema"] != 1 or metadata["input_sha256"] != expected_inputs
                or metadata["compiler"] != compiler):
            raise ValueError("SDK archive does not match the selected inputs/compiler")
        packages = metadata["packages"]
        paths = [cache_path(p["abi"]) for p in packages]
        if not packages or len(paths) != len(set(paths)):
            raise ValueError("Invalid SDK package inventory")
        # Validate the complete inventory before writing anything to the cache.
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            for record, relative in zip(packages, paths):
                if record["path"] != relative:
                    raise ValueError("Invalid SDK package path")
                path = stage / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.open("packages/" + relative) as source, path.open("wb") as target:
                    shutil.copyfileobj(source, target)
                if sha256(path) != record["sha256"]:
                    raise ValueError("SDK package checksum mismatch")
            for relative in paths:
                target = cache / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(stage / relative, target)
    print(f"Restored {len(paths)} archived packages; vcpkg will validate their ABI")


def restore(root, lock_path, cache, compiler):
    if not lock_path.is_file():
        print("No SDK snapshot selected; use the normal cache/build")
        return
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    digest = input_hash(root)
    if lock["schema"] != 1:
        raise ValueError("Unsupported SDK lock format")
    if lock["input_sha256"] != digest or lock["compiler"] != compiler:
        print("SDK snapshot inputs/compiler differ; use the normal cache/build")
        return
    tag, asset = lock["tag"], lock["asset"]
    if not re.fullmatch(r"sdk-[a-zA-Z0-9_.-]+", tag) or not re.fullmatch(r"[a-zA-Z0-9_.-]+\.zip", asset):
        raise ValueError("Invalid SDK release/asset")
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / asset
        try:
            with urllib.request.urlopen(RELEASES + tag + "/" + asset, timeout=60) as source, archive.open("wb") as target:
                shutil.copyfileobj(source, target)
        except OSError as error:
            print(f"SDK snapshot unavailable ({error}); use the normal cache/build")
            return
        restore_archive(archive, cache, lock["sha256"], digest, compiler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["pack", "restore"])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache/vcpkg")
    parser.add_argument("--installed", type=Path, default=ROOT / "vcpkg_installed")
    parser.add_argument("--output", type=Path, default=ROOT / "out/windows-sdk-2026.1.zip")
    parser.add_argument("--lock", type=Path, default=ROOT / "cmake/windows-sdk.lock.json")
    args = parser.parse_args()
    if args.command == "pack":
        pack(args.root, args.cache, args.installed, args.output)
    else:
        restore(args.root, args.lock, args.cache, os.environ["VCToolsVersion"].rstrip("/\\"))


if __name__ == "__main__":
    main()
