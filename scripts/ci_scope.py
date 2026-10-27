#!/usr/bin/env python3
"""Reuse successful CI only when every build input is unchanged. No dependencies."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build_fingerprint(root=ROOT, revision="HEAD"):
    tree = subprocess.check_output(
        ["git", "ls-tree", "-r", "-z", "--full-tree", revision], cwd=root)
    digest = hashlib.sha256()
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        _, path = entry.split(b"\t", 1)
        if path.startswith((b"docs/", b"design/")) or path.lower().endswith(b".md"):
            continue
        # Include path, mode and blob ID: renames/deletions/permissions matter too.
        digest.update(entry + b"\0")
    return digest.hexdigest()


def needs_build(event, ref_type, fingerprint, proof):
    if ref_type != "branch" or event not in {"pull_request", "push"}:
        return True
    try:
        return proof.read_text(encoding="ascii").strip() != fingerprint
    except (OSError, UnicodeError):
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--proof", type=Path)
    args = parser.parse_args()
    fingerprint = build_fingerprint()
    outputs = {"fingerprint": fingerprint}
    if args.proof is not None:
        ref_type = os.environ.get("GITHUB_REF_TYPE", "")
        build = needs_build(os.environ.get("GITHUB_EVENT_NAME", ""), ref_type,
                            fingerprint, args.proof)
        outputs.update(build=str(build).lower(), package=str(ref_type == "tag").lower())
        print("Full build required" if build else "Reusing successful CI for unchanged build inputs")
    with args.github_output.open("a", encoding="utf-8") as output:
        for name, value in outputs.items():
            output.write(f"{name}={value}\n")


if __name__ == "__main__":
    main()
