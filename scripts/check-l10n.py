#!/usr/bin/env python3
"""Localization checks for the macOS app (run by scripts/test-macos.sh).

1. No Japanese text is hard-coded in apps/macos/Sources (Japanese lives only in the String Catalog).
2. Every entry of apps/macos/Resources/Localizable.xcstrings has a translated Japanese value whose
   format specifiers match the English key (same types in the same order, positional or not).
3. With --stringsdata <dir>: every string the Swift compiler extracted for localization (the
   *.stringsdata files of an app build) has an entry in the catalog, and every catalog entry is
   used. Keys without any letters (e.g. "%lld", "#%llu") need no translation and are skipped.
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "apps", "macos")
JAPANESE = re.compile(r"[぀-ヿ㐀-䶿一-鿿＀-￯]")
SPEC = re.compile(r"%(?:(\d+)\$)?([-+#0]*\d*(?:\.\d+)?(?:ll|l|h|hh|q|z|t|j)?[@dDiuUxXoOfFeEgGcCsSpaA%])")

def specs(s):
    out, seq = [], 0
    for m in SPEC.finditer(s):
        conv = m.group(2)
        if conv == "%":
            continue
        typ = re.sub(r"^[-+#0]*\d*(?:\.\d+)?", "", conv)
        if m.group(1):
            out.append((int(m.group(1)), typ))
        else:
            seq += 1
            out.append((seq, typ))
    return sorted(out)

def main():
    errors = []
    for path in sorted(glob.glob(os.path.join(APP, "Sources", "**", "*.swift"), recursive=True)):
        for n, line in enumerate(open(path, encoding="utf-8"), 1):
            if JAPANESE.search(line):
                errors.append(f"{os.path.relpath(path, ROOT)}:{n}: Japanese text outside the String Catalog")
    cat = json.load(open(os.path.join(APP, "Resources", "Localizable.xcstrings"), encoding="utf-8"))
    if cat.get("sourceLanguage") != "en":
        errors.append("Localizable.xcstrings: sourceLanguage must be en")
    strings = cat["strings"]
    for key, entry in strings.items():
        unit = entry.get("localizations", {}).get("ja", {}).get("stringUnit")
        if not unit or unit.get("state") != "translated" or not unit.get("value"):
            errors.append(f"catalog: no Japanese translation for {key!r}")
            continue
        if specs(key) != specs(unit["value"]):
            errors.append(f"catalog: format specifiers differ for {key!r}: {unit['value']!r}")
    if "--stringsdata" in sys.argv:
        d = sys.argv[sys.argv.index("--stringsdata") + 1]
        files = glob.glob(os.path.join(d, "**", "*.stringsdata"), recursive=True)
        if not files:
            errors.append(f"no .stringsdata files under {d}")
        seen = set()
        for f in files:
            data = json.load(open(f, encoding="utf-8"))
            for table, items in data.get("tables", {}).items():
                for it in items:
                    key = it["key"]
                    if key in seen:
                        continue
                    seen.add(key)
                    if not re.search(r"[A-Za-z]", SPEC.sub("", key)):
                        continue
                    if key not in strings:
                        src = os.path.relpath(data.get("source", "?"), ROOT)
                        errors.append(f"{src}:{it['location']['startingLine']}: {key!r} is missing from Localizable.xcstrings")
        for key in sorted(set(strings) - seen):
            errors.append(f"catalog: {key!r} is not used by the app (stale key or wrong format specifier)")
    for e in errors:
        print(e)
    print(f"check-l10n: {len(strings)} catalog entries, {len(errors)} problem(s)")
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
