# Bundled brand fonts

The dock, the report dialog and the live overlay use the Trigglow web's typefaces (headlines in
**Archivo**, labels and figures in **Geist Mono**). They are bundled as small static subsets in
`data/fonts/` and registered by `src/ui-theme.cpp` (`LoadBrandFonts()`) for the plugin's own widgets
only; OBS's UI font is never changed. If they are missing, every role falls back to OBS's font and
the log says so once.

| File in `data/fonts/` | Source (github.com/google/fonts, `main`) | Instance |
|---|---|---|
| `Archivo-SemiBold.ttf` | `ofl/archivo/Archivo[wdth,wght].ttf`, git blob `cc64253d36665a5ca0d6719cdf1e32b3de453b51` | `wght` 600, `wdth` 100 |
| `Archivo-Bold.ttf` | same file | `wght` 700, `wdth` 100 |
| `GeistMono-Medium.ttf` | `ofl/geistmono/GeistMono[wght].ttf`, git blob `173867dce0580ea751e4b9c558740d34f20ce549` | `wght` 500 |
| `OFL-Archivo.txt`, `OFL-GeistMono.txt` | the `OFL.txt` next to each source font | SIL Open Font License 1.1 |

Both fonts are under the SIL Open Font License 1.1 with no Reserved Font Names, so the subsets may
keep their names. The OFL text must travel with the fonts: keep each `OFL-*.txt` next to them
(`installers/windows/trigglow-dynamic-delay-setup.iss` and CMake's `data/` install copy both).

## Regenerating

`make_fonts.py` (needs fontTools, tested with 4.60.1) instances the variable fonts and subsets them:

```
python -P make_fonts.py <folder with the two upstream .ttf files> <output folder>
```

- Characters: Basic Latin, Latin-1 Supplement (with the no-break space U+00A0 the UI and the overlay
  put in "30 s"), Latin Extended-A, plus the thin space U+2009 (Archivo only: Geist Mono has none),
  en/em dashes, curly quotes, ellipsis, bullet, euro and trademark. Neither upstream font has the
  narrow no-break space U+202F, so the strings never use it, and the overlay draws any space in its
  value as U+00A0 (`src/overlay-badge.cpp`). To check a character, e.g. U+202F:
  `python -P -c "from fontTools.ttLib import TTFont; print(0x202F in TTFont('data/fonts/Archivo-Bold.ttf').getBestCmap())"`.
- OpenType features kept: `kern liga calt ccmp locl mark mkmk tnum case` (`tnum` gives the overlay's
  tabular figures).
- Names: typographic family "Archivo" / "Geist Mono" with styles "SemiBold", "Bold", "Medium";
  legacy (GDI) family "Archivo SemiBold" / "Geist Mono Medium" for the non-RIBBI styles, whose legacy
  style is "Regular". The style bits follow it: `fsSelection` BOLD and `macStyle` bold on
  Archivo-Bold, `fsSelection` REGULAR on the SemiBold and Medium files, never ITALIC. The plugin
  never hardcodes a family name: it reads what Qt registered
  (`QFontDatabase::applicationFontFamilies()`), which on Windows/DirectWrite is the typographic one.

Copy the three `.ttf` files and both licenses into `data/fonts/` and rebuild; nothing else changes.
