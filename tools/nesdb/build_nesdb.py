#!/usr/bin/env python3
"""Build frontend/data/nesdb.tsv, the offline NES/Famicom game database.

Sources:
  * Wikidata (CC0), cached in tools/nesdb/cache/wikidata-*.json.
    The network is only touched with --refresh.
  * frontend/data/nesdb-overrides.json (hand-written fixes, extra games, hash keys).
  * third_party/nestopia/NstDatabase.xml, only to validate override hash keys and
    derive their region.

Usage:
  python3 tools/nesdb/build_nesdb.py             # rebuild nesdb.tsv from the cache
  python3 tools/nesdb/build_nesdb.py --refresh   # re-query Wikidata, then rebuild
  python3 tools/nesdb/build_nesdb.py --report    # rebuild + print/write coverage

Python 3 standard library only.
"""

import argparse
import json
import os
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CACHE_DIR = os.path.join(HERE, "cache")
OVERRIDES = os.path.join(ROOT, "frontend", "data", "nesdb-overrides.json")
OUTPUT = os.path.join(ROOT, "frontend", "data", "nesdb.tsv")
NSTDB = os.path.join(ROOT, "third_party", "nestopia", "NstDatabase.xml")
COVERAGE = os.path.join(HERE, "COVERAGE.md")

SPARQL_URL = "https://query.wikidata.org/sparql"
USER_AGENT = "ReplayNES-nesdb/1.0 (https://github.com/kathoc/ReplayNES)"

# Q172742 = Nintendo Entertainment System (its ja label is ファミリーコンピュータ),
# Q491640 = Family Computer (separate item, few games use it).
# Famicom Disk System (Q135321) is deliberately not included: disk-only games
# drop out, games also released on cartridge carry Q172742 as well.
PLATFORMS = "wd:Q172742 wd:Q491640"
BASE = "VALUES ?p { %s } ?i wdt:P400 ?p ." % PLATFORMS

QUERIES = {
    # "mul" (multiple languages) labels replace identical en labels on many items.
    "items": """SELECT DISTINCT ?i ?en ?ja ?mul WHERE { %s
  OPTIONAL { ?i rdfs:label ?en FILTER(LANG(?en) = "en") }
  OPTIONAL { ?i rdfs:label ?mul FILTER(LANG(?mul) = "mul") }
  OPTIONAL { ?i rdfs:label ?ja FILTER(LANG(?ja) = "ja") } }""" % BASE,
    "types": """SELECT DISTINCT ?i ?t WHERE { %s ?i wdt:P31 ?t . }""" % BASE,
    "aliases": """SELECT DISTINCT ?i ?a (LANG(?a) AS ?l) WHERE { %s
  ?i skos:altLabel ?a FILTER(LANG(?a) = "en" || LANG(?a) = "ja" || LANG(?a) = "mul") }""" % BASE,
    "kana": """SELECT DISTINCT ?i ?k WHERE { %s ?i wdt:P1814 ?k . }""" % BASE,
    "publishers": """SELECT DISTINCT ?i ?pub ?rank ?place ?plat ?en ?mul ?ja ?inc WHERE { %s
  ?i p:P123 ?st . ?st ps:P123 ?pub ; wikibase:rank ?rank .
  FILTER(?rank != wikibase:DeprecatedRank)
  OPTIONAL { ?st pq:P291 ?place }
  OPTIONAL { ?st pq:P400 ?plat }
  OPTIONAL { ?pub wdt:P571 ?inc }
  OPTIONAL { ?pub rdfs:label ?en FILTER(LANG(?en) = "en") }
  OPTIONAL { ?pub rdfs:label ?mul FILTER(LANG(?mul) = "mul") }
  OPTIONAL { ?pub rdfs:label ?ja FILTER(LANG(?ja) = "ja") } }""" % BASE,
    "dates": """SELECT DISTINCT ?i ?d ?place ?plat WHERE { %s
  ?i p:P577 ?st . ?st psv:P577 ?v . ?v wikibase:timeValue ?d .
  ?st wikibase:rank ?rank FILTER(?rank != wikibase:DeprecatedRank)
  OPTIONAL { ?st pq:P291 ?place }
  OPTIONAL { ?st pq:P400 ?plat } }""" % BASE,
    "genres": """SELECT DISTINCT ?i ?g ?gl WHERE { %s ?i wdt:P136 ?g .
  OPTIONAL { ?g rdfs:label ?gl FILTER(LANG(?gl) = "en") } }""" % BASE,
    # Japanese Wikipedia article title: ja title fallback when the ja label is missing.
    "jawiki": """SELECT DISTINCT ?i ?n WHERE { %s
  ?a schema:about ?i ; schema:isPartOf <https://ja.wikipedia.org/> ; schema:name ?n . }""" % BASE,
    # P1476 (title) in Japanese.
    "titles": """SELECT DISTINCT ?i ?t WHERE { %s ?i wdt:P1476 ?t . FILTER(LANG(?t) = "ja") }""" % BASE,
}

# ---------------------------------------------------------------- network


def sparql(query):
    data = urllib.parse.urlencode({"query": query, "format": "json"}).encode()
    req = urllib.request.Request(SPARQL_URL, data=data, headers={
        "User-Agent": USER_AGENT,
        "Accept": "application/sparql-results+json",
        "Content-Type": "application/x-www-form-urlencoded",
    })
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=240) as r:
                return json.load(r)["results"]["bindings"]
        except Exception as e:  # noqa: BLE001 - retry any transient failure
            last = e
            print("  retry after error: %s" % e, file=sys.stderr)
            time.sleep(5 * (attempt + 1))
    raise SystemExit("SPARQL query failed: %s" % last)


def year_of(d):
    m = re.match(r"^[+]?(\d{4})-", d or "")
    return m.group(1) if m else ""


def qid(uri):
    return uri.rsplit("/", 1)[-1]


def val(b, k):
    v = b.get(k)
    return v["value"] if v else ""


def write_cache(name, rows):
    """rows: list of lists of strings. Written sorted, one row per line."""
    rows = sorted(set(tuple(r) for r in rows))
    path = os.path.join(CACHE_DIR, "wikidata-%s.json" % name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("[\n")
        f.write(",\n".join(json.dumps(list(r), ensure_ascii=False, separators=(",", ":")) for r in rows))
        f.write("\n]\n")
    print("  wrote %s (%d rows)" % (os.path.relpath(path, ROOT), len(rows)))


def refresh(names=None):
    os.makedirs(CACHE_DIR, exist_ok=True)
    for name, q in QUERIES.items():
        if names and name not in names:
            continue
        print("querying %s ..." % name)
        rows = []
        for b in sparql(q):
            i = qid(val(b, "i"))
            if name == "items":
                rows.append([i, val(b, "en"), val(b, "ja"), val(b, "mul")])
            elif name == "types":
                rows.append([i, qid(val(b, "t"))])
            elif name == "aliases":
                rows.append([i, val(b, "l"), val(b, "a")])
            elif name == "kana":
                rows.append([i, val(b, "k")])
            elif name == "publishers":
                rank = qid(val(b, "rank"))
                rows.append([i, qid(val(b, "pub")), "P" if rank == "PreferredRank" else "N",
                             qid(val(b, "place")) if val(b, "place") else "",
                             qid(val(b, "plat")) if val(b, "plat") else "",
                             val(b, "en") or val(b, "mul"), val(b, "ja"),
                             year_of(val(b, "inc"))])
            elif name == "dates":
                rows.append([i, year_of(val(b, "d")),
                             qid(val(b, "place")) if val(b, "place") else "",
                             qid(val(b, "plat")) if val(b, "plat") else ""])
            elif name == "genres":
                rows.append([i, qid(val(b, "g")), val(b, "gl")])
            elif name == "jawiki":
                rows.append([i, val(b, "n")])
            elif name == "titles":
                rows.append([i, val(b, "t")])
        if name == "publishers":
            # Keep one row per statement: the earliest inception year of the publisher.
            best = {}
            for r in rows:
                k = tuple(r[:7])
                if k not in best or (r[7] and (not best[k] or r[7] < best[k])):
                    best[k] = r[7]
            rows = [list(k) + [v] for k, v in best.items()]
        write_cache(name, rows)
        time.sleep(2)


def load_cache(name):
    path = os.path.join(CACHE_DIR, "wikidata-%s.json" % name)
    if not os.path.exists(path):
        raise SystemExit("missing cache %s; run with --refresh" % path)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- tables

NES_PLATFORMS = {"Q172742", "Q491640"}

# P31 values that make an item a game; items without any P31 are kept too.
GAME_TYPES = {
    "Q7889",       # video game
    "Q123012190",  # unlicensed video game
    "Q123002395",  # homebrew video game
    "Q598205",     # homebrew
    "Q16070115",   # video game compilation
    "Q2910554",    # bootleg video game
    "Q108905501",  # demake
}
# Types that disqualify an item even if it is also typed as a video game.
NON_GAME_TYPES = {
    "Q64139617",   # ROM hack
    "Q61475894",   # cancelled/unreleased video game
}

PLACE_REGION = {
    "Q17": "JP",                                   # Japan
    "Q30": "NA", "Q49": "NA", "Q16": "NA",         # USA, North America, Canada
    "Q2017699": "NA",                              # Northern America
    "Q46": "EU", "Q458": "EU", "Q145": "EU",       # Europe, EU, UK
    "Q183": "EU", "Q142": "EU", "Q38": "EU",       # Germany, France, Italy
    "Q29": "EU", "Q55": "EU", "Q34": "EU",         # Spain, Netherlands, Sweden
    "Q21195": "EU", "Q2729044": "EU",              # Scandinavia, PAL region
    "Q408": "EU",                                  # Australia (PAL)
    "Q13780930": "JP,NA,EU",                       # worldwide
}
REGION_ORDER = ["JP", "NA", "EU"]

NST_REGION = {"Famicom": "JP", "NES-NTSC": "NA"}  # NES-PAL* -> EU

GENRE_CODES = ["action", "shooter", "puzzle", "rpg", "adventure", "sports", "racing",
               "fighting", "strategy", "table", "music", "educational", "other"]
# Tie-break when two genres are equally specific (earlier wins).
GENRE_PRIORITY = ["rpg", "shooter", "fighting", "racing", "sports", "puzzle", "table",
                  "music", "educational", "strategy", "adventure", "action", "other"]

# Wikidata genre Q-id -> (code, specificity). Specificity 1 = generic umbrella genre.
GENRE_MAP = {
    "Q270948": ("action", 1), "Q828322": ("action", 2), "Q116790414": ("action", 2),
    "Q136849242": ("action", 2), "Q343568": ("action", 2), "Q401831": ("action", 2),
    "Q19643088": ("action", 2), "Q13717398": ("action", 2), "Q1163960": ("action", 2),
    "Q858523": ("action", 2), "Q2070892": ("action", 2), "Q2941225": ("action", 2),
    "Q66503439": ("action", 2), "Q1223895": ("action", 1), "Q15613992": ("action", 1),
    "Q4282636": ("shooter", 1), "Q1044478": ("shooter", 2), "Q1037904": ("shooter", 2),
    "Q96146237": ("shooter", 2), "Q96146141": ("shooter", 2), "Q60480500": ("shooter", 2),
    "Q3965952": ("shooter", 2), "Q185029": ("shooter", 2), "Q380266": ("shooter", 2),
    "Q2127647": ("shooter", 2), "Q107527031": ("shooter", 2), "Q63106609": ("shooter", 2),
    "Q66230650": ("shooter", 2), "Q603481": ("shooter", 2),
    "Q54767": ("puzzle", 2), "Q10308060": ("puzzle", 2), "Q11282930": ("puzzle", 2),
    "Q62591185": ("puzzle", 2), "Q7802107": ("puzzle", 2), "Q124331614": ("puzzle", 2),
    "Q744038": ("rpg", 2), "Q1422746": ("rpg", 2), "Q5923834": ("rpg", 2),
    "Q1265717": ("rpg", 2),
    "Q23916": ("adventure", 1), "Q689445": ("adventure", 2), "Q11590806": ("adventure", 2),
    "Q1635956": ("adventure", 2), "Q1201679": ("adventure", 2), "Q333967": ("adventure", 2),
    "Q868217": ("sports", 2), "Q1478420": ("sports", 2), "Q63915391": ("sports", 2),
    "Q63645120": ("sports", 2), "Q62019045": ("sports", 2), "Q105221690": ("sports", 2),
    "Q61793886": ("sports", 2), "Q71474750": ("sports", 2), "Q3177953": ("sports", 2),
    "Q63915027": ("sports", 2), "Q71468194": ("sports", 2), "Q62019097": ("sports", 2),
    "Q2016457": ("sports", 2), "Q61719251": ("sports", 2), "Q63645079": ("sports", 2),
    "Q71467408": ("sports", 2), "Q62019077": ("sports", 2), "Q71467370": ("sports", 2),
    "Q63915650": ("sports", 2), "Q63915162": ("sports", 2), "Q60256879": ("sports", 2),
    "Q860750": ("racing", 2), "Q108611897": ("racing", 2),
    "Q846224": ("fighting", 2),
    "Q472055": ("strategy", 1), "Q1610017": ("strategy", 1), "Q2176159": ("strategy", 2),
    "Q1529437": ("strategy", 2), "Q4375640": ("strategy", 2), "Q2454898": ("strategy", 2),
    "Q3139142": ("strategy", 2), "Q208189": ("strategy", 2), "Q1199309": ("strategy", 2),
    "Q1198141": ("strategy", 2), "Q588289": ("strategy", 2), "Q3775539": ("strategy", 2),
    "Q124695018": ("strategy", 2),
    "Q19272838": ("table", 2), "Q3742351": ("table", 2), "Q9318902": ("table", 2),
    "Q130374784": ("table", 2), "Q71679871": ("table", 2), "Q60617948": ("table", 2),
    "Q11298493": ("table", 2), "Q60617905": ("table", 2), "Q7888616": ("table", 2),
    "Q653928": ("table", 2), "Q60617897": ("table", 2), "Q14947863": ("table", 2),
    "Q7832342": ("table", 2),
    "Q584105": ("music", 2),
    "Q1224999": ("educational", 2), "Q1153173": ("educational", 2),
    "Q134929728": ("educational", 2), "Q60616268": ("educational", 2),
    "Q4740553": ("other", 2), "Q1080071": ("other", 2), "Q4451239": ("other", 2),
    "Q1892535": ("other", 2), "Q61507686": ("other", 2), "Q11064557": ("other", 2),
    "Q2111958": ("other", 2), "Q749749": ("other", 1),
}

# Fallback on the English genre label (only labels that name a game genre).
GENRE_KEYWORDS = [
    ("role-playing", "rpg"), ("rpg", "rpg"), ("shoot", "shooter"), ("gun", "shooter"),
    ("puzzle", "puzzle"), ("racing", "racing"), ("driving", "racing"),
    ("fighting", "fighting"), ("platform", "action"), ("beat 'em up", "action"),
    ("football", "sports"), ("baseball", "sports"), ("basketball", "sports"),
    ("golf", "sports"), ("tennis", "sports"), ("wrestling", "sports"), ("boxing", "sports"),
    ("hockey", "sports"), ("soccer", "sports"), ("sport", "sports"),
    ("strategy", "strategy"), ("simulation", "strategy"), ("wargame", "strategy"),
    ("tactic", "strategy"), ("board", "table"), ("card", "table"), ("mahjong", "table"),
    ("shogi", "table"), ("chess", "table"), ("pachinko", "table"), ("casino", "table"),
    ("quiz", "table"), ("trivia", "table"), ("party", "table"), ("music", "music"),
    ("rhythm", "music"), ("educat", "educational"), ("adventure", "adventure"),
    ("action", "action"),
]

# ---------------------------------------------------------------- text helpers

_DISAMBIG = re.compile(
    r"\s*[(（][^()（）]*(?:video ?game|game|NES|Famicom|ファミコン|ファミリーコンピュータ|"
    r"ゲーム|ソフト|企業|会社)[^()（）]*[)）]\s*$", re.I)


def clean(s):
    s = re.sub(r"[\t\r\n]+", " ", s or "")
    return re.sub(r"\s{2,}", " ", s).strip()


def clean_title(s):
    s = clean(s)
    prev = None
    while prev != s:
        prev = s
        s = _DISAMBIG.sub("", s).strip()
    return s


def to_katakana(s):
    return "".join(chr(ord(c) + 0x60) if 0x3041 <= ord(c) <= 0x3096 else c for c in s)


def _is_kata(c):
    o = ord(c)
    return 0x30A1 <= o <= 0x30FA or o in (0x30FC, 0x30FD, 0x30FE)


def _is_num(c):
    return "0" <= c <= "9"


def _is_punct(c):
    return c == "・" or unicodedata.category(c)[0] in "PSZ"


def kana_only(s):
    """Return the katakana reading if s is written entirely in kana/numbers/symbols."""
    if not s:
        return ""
    s = to_katakana(unicodedata.normalize("NFKC", s))
    out = []
    for c in s:
        if _is_kata(c) or _is_num(c):
            out.append(c)
        elif _is_punct(c):
            continue
        else:
            return ""
    r = "".join(out)
    return r if any(_is_kata(c) for c in r) else ""


def _is_kanji(c):
    o = ord(c)
    return 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF or c in "々〆ヶ"


def alias_reading_ok(title, reading):
    """A kana alias is used as the reading of a kanji title only if the title's kana
    parts appear in it in order (so 悪魔城ドラキュラ does not take キャッスルバニア).
    A title with no Japanese script at all (MOTHER, FLAPPY) takes any kana alias."""
    t = to_katakana(unicodedata.normalize("NFKC", title))
    if not any(_is_kata(c) or _is_kanji(c) for c in t):
        return True
    parts, has_anchor = [], False
    for c in t:
        if _is_kanji(c):
            if not parts or parts[-1] != ".+":
                parts.append(".+")
        elif _is_kata(c) or _is_num(c):
            parts.append(re.escape(c))
            has_anchor = has_anchor or _is_kata(c)
        elif _is_punct(c):
            continue
        else:
            return False  # Latin letters etc.: cannot verify
    if not has_anchor:
        return False
    return re.fullmatch("".join(parts), reading) is not None


def qnum(key):
    return int(key[1:]) if re.fullmatch(r"Q\d+", key) else 1 << 62


def id_key(key):
    return (0, qnum(key), "") if re.fullmatch(r"Q\d+", key) else (1, 0, key)


def region_str(regions):
    s = set()
    for r in regions:
        s.update(x for x in r.split(",") if x)
    return ",".join(r for r in REGION_ORDER if r in s)


def genre_of(genres):
    best = None
    for gq, label in genres:
        hit = GENRE_MAP.get(gq)
        if not hit:
            low = (label or "").lower()
            if "game" in low or "video" in low:
                for kw, code in GENRE_KEYWORDS:
                    if kw in low:
                        hit = (code, 1)
                        break
        if not hit:
            continue
        score = (-hit[1], GENRE_PRIORITY.index(hit[0]))
        if best is None or score < best[0]:
            best = (score, hit[0])
    return best[1] if best else ""


# ---------------------------------------------------------------- nestopia db


def load_nst():
    db = {}
    if not os.path.exists(NSTDB):
        return db
    for cart in ET.parse(NSTDB).getroot().iter("cartridge"):
        crc = (cart.get("crc") or "").upper()
        if crc:
            db.setdefault(crc, []).append(((cart.get("sha1") or "").upper(), cart.get("system") or ""))
    return db


def nst_region(system):
    if system in NST_REGION:
        return NST_REGION[system]
    return "EU" if system.startswith("NES-PAL") else ""


# ---------------------------------------------------------------- build


def build():
    with open(OVERRIDES, encoding="utf-8") as f:
        ov = json.load(f)
    ov_games = ov.get("games", {})
    ov_pubs = ov.get("publishers", {})
    exclude = set(ov.get("exclude", []))

    types, aliases, kana, pubs, dates, genres, jawiki, titles = ({} for _ in range(8))
    for i, t in load_cache("types"):
        types.setdefault(i, set()).add(t)
    for i, lang, a in load_cache("aliases"):
        aliases.setdefault(i, []).append((lang, a))
    for i, k in load_cache("kana"):
        kana.setdefault(i, []).append(k)
    for row in load_cache("publishers"):
        pubs.setdefault(row[0], []).append(row[1:])
    for row in load_cache("dates"):
        dates.setdefault(row[0], []).append(row[1:])
    for i, g, gl in load_cache("genres"):
        genres.setdefault(i, []).append((g, gl))
    for i, n in load_cache("jawiki"):
        jawiki.setdefault(i, []).append(n)
    for i, t in load_cache("titles"):
        titles.setdefault(i, []).append(t)

    pub_names = {}   # key -> [en, ja]
    games = {}
    for i, en, ja, mul in load_cache("items"):
        t = types.get(i, set())
        if i in exclude or (t and not (t & GAME_TYPES)) or (t & NON_GAME_TYPES):
            continue
        g = {"en": clean_title(en or mul), "ja": clean_title(ja)}
        if not g["ja"]:
            for cand in sorted(titles.get(i, [])) + sorted(jawiki.get(i, [])):
                if clean_title(cand):
                    g["ja"] = clean_title(cand)
                    break
        g["kana"] = sorted(kana.get(i, []))
        # en/mul aliases first (romanized titles help file-name matching), then ja.
        g["aliases"] = [a for _, a in sorted(aliases.get(i, []), key=lambda x: (x[0] == "ja", x[1]))]
        # Release dates: statements qualified with the NES/Famicom platform. Japanese
        # Famicom releases are often recorded without a platform qualifier, so when no
        # Japanese NES/Famicom date exists, unqualified dates from 1983 on count too
        # (earlier ones are arcade/computer originals).
        ds = [d for d in dates.get(i, []) if d[0]]
        nes_ds = [d for d in ds if d[2] in NES_PLATFORMS]
        if not any(d[1] == "Q17" for d in nes_ds):
            nes_ds += [d for d in ds if not d[2] and int(d[0]) >= 1983]
        years = [int(d[0]) for d in nes_ds]
        g["year"] = min(years) if years else 0
        regs = [PLACE_REGION.get(d[1], "") for d in nes_ds]
        # Publisher: drop statements qualified with another platform, then rank:
        # preferred rank, Japanese release, NES/Famicom-qualified, publisher existed
        # when the game came out (not a later successor company), lowest Q-id.
        ps = [p for p in pubs.get(i, []) if not p[3] or p[3] in NES_PLATFORMS]
        if ps:
            gy = g["year"] or 9999
            ps.sort(key=lambda p: (p[1] != "P", p[2] != "Q17", p[3] not in NES_PLATFORMS,
                                   bool(p[6]) and int(p[6]) > gy, qnum(p[0])))
            g["publisher"] = ps[0][0]
            for p in ps:
                pub_names.setdefault(p[0], [clean_title(p[4]), clean_title(p[5])])
                regs.append(PLACE_REGION.get(p[2], ""))
        else:
            g["publisher"] = ""
        g["region"] = region_str(regs)
        g["genre"] = genre_of(sorted(genres.get(i, [])))
        if not g["en"] and not g["ja"]:
            continue
        games[i] = g

    # Overrides: add or replace fields.
    for gid, o in ov_games.items():
        if gid in exclude:
            continue
        g = games.get(gid)
        if g is None and not (o.get("en") or o.get("ja")):
            print("warning: override %s matches no game and has no title; skipped" % gid,
                  file=sys.stderr)
            continue
        if g is None:
            g = games[gid] = {"en": "", "ja": "", "kana": [], "aliases": [], "year": 0,
                              "publisher": "", "region": "", "genre": ""}
        for k in ("en", "ja", "publisher", "region", "genre"):
            if k in o:
                g[k] = clean_title(o[k]) if k in ("en", "ja") else o[k]
        if "year" in o:
            g["year"] = int(o["year"])
        if "reading" in o:
            g["reading_override"] = o["reading"]
        if "aliases" in o:
            g["aliases"] = list(o["aliases"]) + g["aliases"]
    for k, v in ov_pubs.items():
        cur = pub_names.setdefault(k, ["", ""])
        if "en" in v:
            cur[0] = v["en"]
        if "ja" in v:
            cur[1] = v["ja"]

    # Hash keys, validated against NstDatabase.xml.
    nst = load_nst()
    hashes, hash_stats = [], {"matched": 0, "unmatched": [], "dangling": []}
    for h in ov.get("hashes", []):
        crc = h["crc32"].upper()
        sha1 = (h.get("sha1") or "").upper()
        gid = h["game"]
        if gid not in games:
            hash_stats["dangling"].append(crc)
            continue
        hits = [s for s in nst.get(crc, []) if not sha1 or s[0] == sha1]
        if hits:
            hash_stats["matched"] += 1
            g = games[gid]
            g["region"] = region_str([g["region"]] + [nst_region(s[1]) for s in hits])
        else:
            hash_stats["unmatched"].append(crc)
        hashes.append((crc, sha1, gid))

    # Finalise every game.
    for gid, g in games.items():
        if g["genre"] not in GENRE_CODES:
            g["genre"] = ""
        has_ja = bool(g["ja"])
        if not has_ja:
            g["ja"] = g["en"]
        # Reading: override > P1814 > kana-only ja title > verified kana-only ja alias
        # (aliases only when a real ja title exists; otherwise a kana alias is usually
        # a different, Japanese release title, e.g. Menace Beach / ミスピーチワールド).
        reading = kana_only(g.get("reading_override") or "")
        if not reading:
            for k in g["kana"]:
                reading = kana_only(k)
                if reading:
                    break
        if not reading:
            reading = kana_only(g["ja"])
        if not reading and has_ja:
            for a in g["aliases"]:
                r = kana_only(a)
                if r and alias_reading_ok(g["ja"], r):
                    reading = r
                    break
        g["reading"] = reading
        seen = {g["en"].casefold(), g["ja"].casefold()}
        out = []
        for a in g["aliases"]:
            a = clean(a).replace("|", "/")
            if not a or a.casefold() in seen or "シリーズ" in a or "(series)" in a.lower():
                continue
            seen.add(a.casefold())
            out.append(a)
        g["aliases"] = out[:8]

    # Deduplicate identical records (keep the lowest id).
    seen, dup = {}, []
    for gid in sorted(games, key=id_key):
        g = games[gid]
        k = (g["en"].casefold(), g["ja"], g["year"])
        if k in seen:
            dup.append(gid)
        else:
            seen[k] = gid
    for gid in dup:
        if not any(h[2] == gid for h in hashes):
            del games[gid]

    lines = ["#nesdb\t1"]
    used = sorted({g["publisher"] for g in games.values() if g["publisher"]}, key=id_key)
    for k in used:
        en, ja = pub_names.get(k, ["", ""])
        lines.append("\t".join(["P", k, clean(en), clean(ja or en)]))
    for gid in sorted(games, key=id_key):
        g = games[gid]
        lines.append("\t".join(["G", gid, clean(g["en"]), clean(g["ja"]), g["reading"],
                                g["publisher"], str(g["year"]), g["genre"], g["region"],
                                "|".join(g["aliases"])]))
    for crc, sha1, gid in sorted(set(hashes)):
        lines.append("\t".join(["H", crc, sha1, gid]))
    data = ("\n".join(lines) + "\n").encode("utf-8")
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(data)
    return games, hashes, hash_stats, len(data)


def report(games, hashes, hash_stats, size):
    n = len(games)
    rows = [
        ("games", n),
        ("ja title distinct from en", sum(1 for g in games.values() if g["ja"] and g["ja"] != g["en"])),
        ("reading", sum(1 for g in games.values() if g["reading"])),
        ("year", sum(1 for g in games.values() if g["year"])),
        ("genre", sum(1 for g in games.values() if g["genre"])),
        ("publisher", sum(1 for g in games.values() if g["publisher"])),
        ("region", sum(1 for g in games.values() if g["region"])),
    ]
    text = ["# nesdb coverage", "",
            "Generated by `python3 tools/nesdb/build_nesdb.py --report`.", "",
            "| field | count | % |", "|---|---:|---:|"]
    for k, v in rows:
        pct = "%.1f" % (100.0 * v / n) if n and k != "games" else ""
        text.append("| %s | %d | %s |" % (k, v, pct))
    text += [
        "",
        "Hash keys: %d (in NstDatabase.xml: %d, not found: %d)"
        % (len(set(hashes)), hash_stats["matched"], len(hash_stats["unmatched"])),
        "",
        "nesdb.tsv size: %d bytes" % size,
    ]
    if hash_stats["unmatched"]:
        text += ["", "Unmatched hash keys: " + ", ".join(sorted(hash_stats["unmatched"]))]
    if hash_stats["dangling"]:
        text += ["", "Hash keys pointing at unknown games: " + ", ".join(sorted(hash_stats["dangling"]))]
    out = "\n".join(text) + "\n"
    print(out, end="")
    with open(COVERAGE, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--refresh", action="store_true", help="re-query Wikidata into the cache")
    ap.add_argument("--report", action="store_true", help="print and write the coverage report")
    args = ap.parse_args()
    if args.refresh:
        refresh()
    games, hashes, hash_stats, size = build()
    print("wrote %s (%d games, %d bytes)" % (os.path.relpath(OUTPUT, ROOT), len(games), size))
    if hash_stats["unmatched"] or hash_stats["dangling"]:
        print("warning: %d hash keys not in NstDatabase.xml, %d point at unknown games"
              % (len(hash_stats["unmatched"]), len(hash_stats["dangling"])), file=sys.stderr)
    if args.report:
        report(games, hashes, hash_stats, size)


if __name__ == "__main__":
    main()
