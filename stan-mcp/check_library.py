"""Check the real document library: its shape, and that search finds what the
facts in hoa_data.py say. python3 stan-mcp/check_library.py"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import library
from hoa_data import COMMUNITIES

L = library.Library()
fails = 0


def check(c, what):
    global fails
    if not c:
        print("FAIL", what)
        fails += 1


# Shape: every community has a library; sections are quotable units.
for key in COMMUNITIES:
    docs = L.docs.get(key, [])
    check(len(docs) >= 6, f"{key}: {len(docs)} documents")
    words = sum(len(s["text"].split()) for d in docs for s in d["sections"])
    secs = sum(len(d["sections"]) for d in docs)
    print(f"{key}: {len(docs)} documents, {secs} sections, {words} words")
    types = {d["type"] for d in docs}
    for t in ("bylaws", "rules", "minutes"):
        check(t in types, f"{key}: has {t}")
    check(types & {"ccrs", "declaration"}, f"{key}: has CC&Rs or a declaration")
    for d in docs:
        check(d["title"] and d["type"], f"{key}/{d['slug']}: title and type")
        for s in d["sections"]:
            n = len(s["text"].split())
            if n < 30 or n > 450:
                check(False, f"{key}/{d['slug']} {s['heading']}: {n} words")

# Search finds the facts.
EXPECT = [
    ("sheppard center", "how tall can my fence be", "6 feet"),
    ("sheppard center", "approved colors I can paint my shed", "Sage"),
    ("sheppard center", "late fee on dues", "$25"),
    ("sheppard center", "when do trash bins go out", "6 pm"),
    ("sheppard center", "short term rental airbnb", "rent"),
    ("stan meadows", "can I put up a fence", "fence"),
    ("stan meadows", "guest parking at the gate", "guest"),
    ("harbor view", "can I barbecue on my balcony", "grill"),
    ("harbor view", "renovation contractor insurance deposit", "deposit"),
    ("oak ridge", "how many horses can I keep", "horse"),
    ("oak ridge", "septic pumping", "septic"),
    ("oak ridge", "who pays for the private roads", "road"),
]
for key, q, want in EXPECT:
    hits = L.search(key, q, k=4)
    ok = any(want.lower() in h["text"].lower() for h in hits)
    check(ok, f"{key}: '{q}' -> {want} (got {[h['cite'] for h in hits]})")

print("library: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
