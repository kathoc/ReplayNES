# nesdb

Generator for `frontend/data/nesdb.tsv`, the offline NES/Famicom game database
(titles in English/Japanese, katakana reading, publisher, year, genre, region,
aliases, and ROM hash keys). The app build never runs this script; the generated
TSV is committed.

## Sources and licences

| source | licence | used for |
|---|---|---|
| Wikidata, cached in `cache/wikidata-*.json` | CC0 | items with platform (P400) Nintendo Entertainment System (Q172742) or Family Computer (Q491640), items whose Japanese Wikipedia article is in カテゴリ:ファミリーコンピュータ用ソフト (category membership only selects items), and every Q-id named in the overrides: labels (en/ja/mul), aliases, P1814 kana name, P2125 Hepburn romanization, P123 publisher, P577 date (+ P291 place / P400 platform qualifiers), P136 genre, P1476 ja title, ja Wikipedia article title |
| `frontend/data/nesdb-overrides.json` | project | hand-written fixes, readings, extra games, hash keys, exclusions |
| `third_party/nestopia/NstDatabase.xml` | GPL-2.0+ | only validates override hash keys and derives their region (Famicom = JP, NES-NTSC = NA, NES-PAL* = EU); no data is copied |

Famicom Disk System (Q135321) is not queried, so disk-only games drop out.
No ROM data is stored; hash keys are CRC32/SHA-1 of PRG+CHR (no iNES header).

## Usage

```sh
python3 tools/nesdb/build_nesdb.py            # rebuild nesdb.tsv from the cache (no network)
python3 tools/nesdb/build_nesdb.py --report   # also print counts and write COVERAGE.md
python3 tools/nesdb/build_nesdb.py --refresh  # re-query Wikidata (query.wikidata.org), rewrite the cache, rebuild
```

Python 3 standard library only. Output is deterministic (sorted, LF, UTF-8).

## Output format (`nesdb.tsv`)

```
#nesdb	1
P	<publisher key>	<publisher en>	<publisher ja>
G	<game id>	<title en>	<title ja>	<reading>	<publisher key>	<year or 0>	<genre>	<region>	<aliases joined by |>
H	<CRC32>	<SHA1 or empty>	<game id>
```

Game id is a Wikidata Q-id or `x-<slug>` for override-only games. Genre is one of
`action shooter puzzle rpg adventure sports racing fighting strategy table music
educational other` or empty. Region is a subset of `JP,NA,EU`.
The ja title falls back to the en title; the publisher ja name falls back to en.

Reading priority: override > Wikidata P1814 > ja title if written only in kana
(hiragana converted to katakana, punctuation dropped, digits kept) > a kana ja alias
that is consistent with the kana parts of the ja title > empty.

For a Japanese title with a reading, the romanized reading (Hepburn as No-Intro
spells it, e.g. 悪魔の招待状 -> `Akumanoshoutaijou`) is appended to the aliases so
"(Japan)" ROM file names match; readings containing loanword marks (ー, ヴ, small
vowels) are skipped. No-Intro titles that differ from that (translations, other
spellings) are hand-added as override `aliases`.

## Overrides (`frontend/data/nesdb-overrides.json`)

```json
{
  "version": 1,
  "publishers": { "Q45700": {"en": "Konami", "ja": "コナミ"} },
  "games": {
    "Q1066035": {"ja": "グラディウスII", "reading": "グラディウスツー"},
    "x-tetris-bps": {"en": "Tetris", "ja": "テトリス", "reading": "テトリス",
                     "publisher": "Q11331181", "year": 1988, "genre": "puzzle",
                     "region": "JP", "aliases": ["Tetris (Bulletproof)"]}
  },
  "hashes": [ {"crc32": "D445F698", "sha1": "FACE...", "game": "Q11168", "note": "Super Mario Bros. (World)"} ],
  "exclude": ["Q71910"]
}
```

Game fields (all optional): `en`, `ja`, `reading`, `publisher` (key), `year`,
`genre`, `region`, `aliases` (prepended to the Wikidata aliases). Any Wikidata Q-id
named in `games` or `hashes` is fetched by `--refresh` even if it is outside the
platform/category set; until then an override for an uncached Q-id needs `en` or
`ja`, otherwise it is skipped with a warning. Hash keys missing from NstDatabase.xml are reported by `--report`.
