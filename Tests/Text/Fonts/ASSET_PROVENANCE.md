# Asset provenance

| Asset | Source and credit | License | Public redistribution |
|---|---|---|---|
| `NotoSansArabic-Regular.ttf` | Noto Sans Arabic 2.009, © 2015–2021 Google LLC; source: https://github.com/notofonts/arabic. | SIL Open Font License 1.1; complete notice in `LICENSE.txt`. | Allowed under the included font license; reserved-name conditions apply to modified fonts. |

## Test subsets

Subsets of Google Noto faces, used only by the Text module tests. They are not shipped.

| File | Source | Kept code points |
|---|---|---|
| `NotoSans-Regular.ttf` | notofonts/noto-fonts @ ffebf8c1, hinted/ttf/NotoSans | Basic Latin, Latin-1, Combining Diacritical Marks, General Punctuation |
| `NotoSansArabic-Regular.ttf` | notofonts/noto-fonts @ ffebf8c1, hinted/ttf/NotoSansArabic | Basic Latin and the Arabic test strings |
| `NotoSansHebrew-Regular.ttf` | notofonts/noto-fonts @ ffebf8c1, hinted/ttf/NotoSansHebrew | Basic Latin and the Hebrew test strings |
| `NotoSansDevanagari-Regular.ttf` | notofonts/noto-fonts @ ffebf8c1, hinted/ttf/NotoSansDevanagari | Basic Latin and the Devanagari test string |
| `NotoSansThai-Regular.ttf` | notofonts/noto-fonts @ ffebf8c1, hinted/ttf/NotoSansThai | Basic Latin and the Thai test strings |
| `NotoSansJP-Regular.otf` | notofonts/noto-cjk @ f8d15753, Sans/SubsetOTF/JP | Basic Latin and the Japanese test strings |

Licenses: `Noto-OFL.txt` (Noto Sans families) and `NotoCJK-OFL.txt` (Noto Sans JP).

To add a code point, re-subset from the source file with all layout features kept:

```
pyftsubset <source> --unicodes=U+0020-007E,<new code points> --layout-features='*' --name-IDs='*' --notdef-outline --glyph-names --output-file=<file>
```
