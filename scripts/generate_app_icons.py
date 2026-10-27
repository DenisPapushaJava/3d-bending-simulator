#!/usr/bin/env python3
"""Regenerate committed app icons (development only: Inkscape and Pillow)."""

from pathlib import Path
import shutil
import subprocess


def main():
    from PIL import Image

    root = Path(__file__).resolve().parents[1]
    folder = root / "src/app/data/icons"
    inkscape = shutil.which("inkscape")
    if not inkscape:
        raise SystemExit("Inkscape is required only to regenerate the committed icons")
    sizes = (16, 24, 32, 48, 64, 128, 256)
    for size in sizes:
        subprocess.run([inkscape, str(folder / "application.svg"), "--export-type=png",
                        f"--export-filename={folder / f'{size}.png'}",
                        f"--export-width={size}", f"--export-height={size}"], check=True)
    with Image.open(folder / "256.png") as image:
        image.save(folder / "application.ico", format="ICO", sizes=[(s, s) for s in sizes])


if __name__ == "__main__":
    main()
