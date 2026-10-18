"""Instance and subset the bundled brand fonts for the OBS plugin.

Usage: python -I make_fonts.py <input_dir> <output_dir>
"""

import sys
from pathlib import Path

from fontTools.subset import Options, Subsetter
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

# Basic Latin, Latin-1 Supplement, Latin Extended-A (Spanish and most European
# languages), plus the typographic punctuation the UI copy uses.
UNICODES = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x180))
    + [
        0x2009,  # thin space
        0x202F,  # narrow no-break space (used in "30 s")
        0x2013, 0x2014,  # en / em dash
        0x2018, 0x2019, 0x201C, 0x201D,  # curly quotes
        0x2026,  # ellipsis
        0x2022,  # bullet (text only, never as an icon)
        0x20AC,  # euro
        0x2122,  # trademark
    ]
)

FEATURES = ["kern", "liga", "calt", "ccmp", "locl", "mark", "mkmk", "tnum", "case"]

JOBS = [
    # (source, axis location, family, style, weight class, output)
    ("Archivo[wdth,wght].ttf", {"wght": 600, "wdth": 100}, "Archivo", "SemiBold", 600, "Archivo-SemiBold.ttf"),
    ("Archivo[wdth,wght].ttf", {"wght": 700, "wdth": 100}, "Archivo", "Bold", 700, "Archivo-Bold.ttf"),
    ("GeistMono[wght].ttf", {"wght": 500}, "Geist Mono", "Medium", 500, "GeistMono-Medium.ttf"),
]


def set_names(font: TTFont, family: str, style: str) -> None:
    """Static-font naming: typographic family + style, legacy family for non-RIBBI."""
    name = font["name"]
    full = f"{family} {style}"
    ps = f"{family.replace(' ', '')}-{style}"
    legacy_family = family if style in ("Regular", "Bold") else full
    legacy_style = style if style in ("Regular", "Bold") else "Regular"
    for rec_id, value in (
        (1, legacy_family),
        (2, legacy_style),
        (4, full),
        (6, ps),
        (16, family),
        (17, style),
    ):
        name.setName(value, rec_id, 3, 1, 0x409)
        name.setName(value, rec_id, 1, 0, 0)
    for rec_id in (3,):
        name.setName(f"{ps};subset-trigglow", rec_id, 3, 1, 0x409)


def main(src_dir: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for src, loc, family, style, weight, out in JOBS:
        font = TTFont(src_dir / src)
        static = instancer.instantiateVariableFont(font, loc, inplace=False)
        static["OS/2"].usWeightClass = weight
        # RIBBI bits: Bold only for the 700 instance; never italic.
        fs = static["OS/2"].fsSelection & ~0b1100001  # clear ITALIC, BOLD, REGULAR
        fs |= 0b100000 if weight == 700 else 0b1000000 if weight == 400 else 0
        static["OS/2"].fsSelection = fs
        mac = static["head"].macStyle & ~0b11
        static["head"].macStyle = mac | (0b1 if weight == 700 else 0)
        set_names(static, family, style)

        opts = Options()
        opts.layout_features = FEATURES
        opts.name_IDs = ["*"]
        opts.name_languages = ["*"]
        opts.notdef_outline = True
        opts.recalc_bounds = True
        opts.drop_tables += ["DSIG"]
        sub = Subsetter(options=opts)
        sub.populate(unicodes=UNICODES)
        sub.subset(static)
        static.save(out_dir / out)
        print(f"{out}\t{(out_dir / out).stat().st_size} bytes")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
