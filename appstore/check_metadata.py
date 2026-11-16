"""Checks the text fields in appstore/metadata.md against App Store Connect's limits.

    python appstore/check_metadata.py
"""
import os
import re

LIMITS = {
    "Name": 30, "Subtitle": 30, "Promotional Text": 170, "Description": 4000,
    "Keywords": 100, "Copyright": 200, "Notes": 4000,
}

text = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "metadata.md"), encoding="utf-8").read()
ok = True
for field, limit in LIMITS.items():
    m = re.search(r"\*\*" + re.escape(field) + r"\*\*[^\n]*\n\n```\n(.*?)\n```", text, re.S)
    if not m:
        print(f"{field}: not found")
        ok = False
        continue
    value = m.group(1)
    over = len(value) > limit
    ok &= not over
    print(f"{field}: {len(value)} / {limit}{'  TOO LONG' if over else ''}")
    if field == "Keywords":
        words = value.split(",")
        dup = {w for w in words if words.count(w) > 1}
        if " " in value or dup:
            print(f"  keywords: no spaces after commas, no repeats ({dup or 'spaces'})")
            ok = False
raise SystemExit(0 if ok else 1)
