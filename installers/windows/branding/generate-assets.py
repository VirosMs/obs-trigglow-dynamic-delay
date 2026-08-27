#!/usr/bin/env python3
"""Regenerates this directory's installer branding assets from the Trigglow
logo. Run manually whenever the source logo changes -- these are checked
into git as plain binary files, not generated at build time, so CI never
needs Pillow or network access to the main monorepo.

Source: apps/web/public/trigglow-logo.png in the main Trigglow monorepo
(streampulse), 1024x1024 RGB. Not vendored into this repo (GPL-2.0-or-later
public repo, kept separate from the private monorepo on purpose -- see
docs/SPEC.md's License section) -- point SOURCE_LOGO below at a local copy
before running.

Requires Pillow: pip install pillow

Size/format requirements below are Inno Setup 6's own documented
recommendations (jrsoftware.org/ishelp), verified 2026-08-27 rather than
guessed:
  - SetupIconFile: standard multi-resolution .ico. No official size list;
    16/24/32/48/64/128/256 covers every context Windows actually asks for
    (taskbar, Explorer list/details/large-icon views, shortcut properties).
  - WizardImageFile: must keep a 164:314 aspect ratio at any resolution;
    240x459 is Inno's own built-in default size, so it's known-safe. PNG
    (with transparency) is supported directly, no BMP conversion needed.
  - WizardSmallImageFile: must be square; built-in default is 147x147,
    anything above 58x58 avoids blurriness up to 250% DPI. 256x256 covers
    that with margin.
"""

from pathlib import Path

from PIL import Image

SOURCE_LOGO = Path(r"E:\Conding\Projects\streampulse\apps\web\public\trigglow-logo.png")
OUT_DIR = Path(__file__).parent

ICON_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
WIZARD_IMAGE_SIZE = (240, 459)  # 164:314 aspect ratio -- see module docstring.
WIZARD_SMALL_SIZE = (256, 256)
WIZARD_IMAGE_LOGO_WIDTH = 176  # How big the (square) logo renders inside the tall banner.


def main() -> None:
    if not SOURCE_LOGO.exists():
        raise SystemExit(f"Source logo not found: {SOURCE_LOGO}\nUpdate SOURCE_LOGO to a local copy.")

    logo = Image.open(SOURCE_LOGO).convert("RGBA")

    # Sample the logo's own inner background (not its flattened, pure-black
    # corners) so the generated banners blend seamlessly with it instead of
    # showing a visible seam against a different dark tone.
    bg = logo.getpixel((logo.width // 2, logo.height // 7))[:3]

    icon_path = OUT_DIR / "setup-icon.ico"
    logo.save(icon_path, sizes=ICON_SIZES)
    print(f"wrote {icon_path.name}")

    small = logo.resize(WIZARD_SMALL_SIZE, Image.LANCZOS)
    small_canvas = Image.new("RGB", WIZARD_SMALL_SIZE, bg)
    small_canvas.paste(small, (0, 0), small)
    small_path = OUT_DIR / "wizard-small.png"
    small_canvas.save(small_path)
    print(f"wrote {small_path.name} {small_canvas.size}")

    banner_w, banner_h = WIZARD_IMAGE_SIZE
    banner = Image.new("RGB", WIZARD_IMAGE_SIZE, bg)
    logo_resized = logo.resize((WIZARD_IMAGE_LOGO_WIDTH, WIZARD_IMAGE_LOGO_WIDTH), Image.LANCZOS)
    x = (banner_w - WIZARD_IMAGE_LOGO_WIDTH) // 2
    y = (banner_h - WIZARD_IMAGE_LOGO_WIDTH) // 2
    banner.paste(logo_resized, (x, y), logo_resized)
    banner_path = OUT_DIR / "wizard-image.png"
    banner.save(banner_path)
    print(f"wrote {banner_path.name} {banner.size}")


if __name__ == "__main__":
    main()
