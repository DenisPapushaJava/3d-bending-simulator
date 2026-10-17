#!/usr/bin/env python3
"""Single release version, tag validation, and GitHub Actions outputs. No dependencies."""

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r"(20\d{2})\.([1-9]\d*)\.(0|[1-9]\d*)(?:-(alpha|beta|rc)\.([1-9]\d*))?")


def parse_version(value):
    match = PATTERN.fullmatch(value)
    if not match:
        raise ValueError("Expected YYYY.release.patch[-alpha.N|-beta.N|-rc.N], e.g. 2026.1.0-alpha.1")
    return {"version": value, "numeric": ".".join(match.group(1, 2, 3)),
            "tag": "v" + value, "prerelease": match.group(4) is not None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-tag", default="")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        info = parse_version((ROOT / "VERSION").read_text(encoding="utf-8").strip())
        manifest = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))
        if manifest["version-string"] != info["version"]:
            raise ValueError("vcpkg.json version-string must equal VERSION")
        if args.check_tag and args.check_tag != info["tag"]:
            raise ValueError(f"Tag {args.check_tag!r} does not match VERSION ({info['tag']})")
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as output:
                for key, value in info.items():
                    output.write(f"{key}={str(value).lower() if isinstance(value, bool) else value}\n")
        print(info["version"])
    except (ValueError, OSError, KeyError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
