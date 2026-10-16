"""The communities' governing documents, searchable: stan-mcp's library.

Each community has a folder under library/ of markdown documents written like
the real thing (CC&Rs, bylaws, rules, architectural guidelines, enforcement
and collection policies, the budget, minutes ...). A document is

    # Title
    <!-- type: ccrs; adopted: 1998-03-12; amended: 2015-06-01 -->
    ## Article 7 - Architectural Control
    ### 7.4 Fences
    Fences may be no taller than 6 feet ...

and every ### section is one searchable unit, quoted with its citation
("CC&Rs, Article 7, Section 7.4"). Search is BM25 over the section's words
(its heading counted twice), with light stemming and an HOA synonym list, so
"can I bbq on my deck" finds the grills section. Plain Python, no service:
the whole library is indexed when the Lambda starts (a few hundred sections).
"""
import math
import os
import re
from collections import Counter

LIBRARY_DIR = os.environ.get("LIBRARY_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "library"))

STOP = set("""a an and are as at be by can could do does for from had has have how i if in into is it its may me might
must my no not of on or our shall should so than that the their them then there these they this to under up was we
were what when where which who will with would you your yours am any about also all been being both each just""".split())

# Words people say -> the words the documents use.
SYNONYMS = {
    "bbq": "grill", "barbecue": "grill", "barbeque": "grill", "smoker": "grill",
    "dog": "pet", "dogs": "pet", "cat": "pet", "cats": "pet", "puppy": "pet", "bark": "pet", "barking": "pet",
    "leash": "pet", "garbage": "trash", "rubbish": "trash", "bin": "trash", "bins": "trash", "recycling": "trash",
    "airbnb": "rental", "vrbo": "rental", "rent": "lease", "renting": "lease", "tenant": "lease", "tenants": "lease",
    "paint": "color", "painted": "color", "colour": "color", "colours": "color", "colors": "color",
    "car": "vehicle", "cars": "vehicle", "truck": "vehicle", "rv": "vehicle", "boat": "vehicle", "trailer": "vehicle",
    "tow": "towing", "towed": "towing", "loud": "noise", "music": "noise", "party": "noise",
    "dues": "assessment", "fee": "assessment", "fees": "assessment", "hoa": "association",
    "fine": "fine", "fined": "fine", "penalty": "fine", "appeal": "hearing", "dispute": "hearing",
    "late": "delinquent", "behind": "delinquent", "owe": "delinquent", "lien": "lien",
    "shed": "accessory", "sheds": "accessory", "deck": "deck", "patio": "patio", "balcony": "balcony",
    "solar": "solar", "panels": "solar", "flag": "flag", "flags": "flag", "sign": "sign", "signs": "sign",
    "christmas": "holiday", "decorations": "holiday", "lights": "holiday", "pool": "pool", "swim": "pool",
    "gym": "fitness", "clubhouse": "clubhouse", "renovate": "renovation", "remodel": "renovation",
    "contractor": "contractor", "horse": "equestrian", "horses": "equestrian", "septic": "septic",
    "meeting": "meeting", "vote": "vote", "voting": "vote", "election": "election", "board": "board",
}


def stem(w):
    """Light stemming, enough to join fence/fences, rule/rules, box/boxes, policy/policies."""
    if len(w) <= 3 or w.endswith(("ss", "us", "is")):
        return w
    if w.endswith("ies"):
        return w[:-3] + "y"
    if w.endswith("es") and w[-3] in "sxz" or w.endswith(("ches", "shes")):
        return w[:-2]
    if w.endswith("s"):
        return w[:-1]
    return w


def words(text):
    out = []
    for w in re.findall(r"[a-z0-9]+", (text or "").lower()):
        if w in STOP or len(w) < 2:
            continue
        out.append(stem(SYNONYMS.get(w, w)))
        if w in SYNONYMS and SYNONYMS[w] != w:        # keep the person's own word too
            out.append(stem(w))
    return out


DOC_NAMES = {"ccrs": "CC&Rs", "declaration": "Declaration", "bylaws": "Bylaws", "rules": "Rules and Regulations",
             "architectural": "Architectural Guidelines", "enforcement": "Fine and Enforcement Policy",
             "collection": "Collection Policy", "budget": "Budget", "minutes": "Minutes"}


def parse(path, community):
    """One markdown document -> its metadata and its ### sections."""
    lines = open(path, encoding="utf-8").read().splitlines()
    doc = {"slug": os.path.splitext(os.path.basename(path))[0], "community": community, "title": "", "type": "",
           "adopted": "", "amended": "", "sections": []}
    article, cur = "", None
    for ln in lines:
        if ln.startswith("# ") and not doc["title"]:
            doc["title"] = ln[2:].strip()
        elif ln.startswith("<!--") and not doc["type"]:
            for part in ln.strip("<!-> ").split(";"):
                if ":" in part:
                    k, v = part.split(":", 1)
                    doc[k.strip()] = v.strip()
        elif ln.startswith("## "):
            article = ln[3:].strip()
        elif ln.startswith("### "):
            cur = {"heading": ln[4:].strip(), "article": article, "lines": []}
            doc["sections"].append(cur)
        elif cur is not None:
            cur["lines"].append(ln)
    for s in doc["sections"]:
        s["text"] = "\n".join(s.pop("lines")).strip()
        m = re.match(r"([0-9]+(?:\.[0-9]+)*)\.?\s+(.*)", s["heading"])
        s["number"], s["title"] = (m.group(1), m.group(2)) if m else ("", s["heading"])
    return doc


def short_name(doc):
    return DOC_NAMES.get(doc["type"], doc["title"])


def cite(doc, s):
    art = re.match(r"(Article|Part|Chapter)\s+([0-9IVXLC]+)", s["article"] or "")
    where = f"{art.group(1)} {art.group(2)}, " if art and doc["type"] != "minutes" else ""
    if doc["type"] == "minutes":
        return f"{doc['title']}, item {s['number'] or s['title']}"
    return f"{short_name(doc)}, {where}Section {s['number']}" if s["number"] else f"{short_name(doc)}, {s['title']}"


class Library:
    K1, B = 1.4, 0.75

    def __init__(self, root=LIBRARY_DIR):
        self.docs = {}                                   # community -> [doc]
        self.units = []                                  # (doc, section, Counter, length)
        if os.path.isdir(root):
            for community in sorted(os.listdir(root)):
                folder = os.path.join(root, community)
                if not os.path.isdir(folder):
                    continue
                key = community.replace("_", " ")
                for f in sorted(os.listdir(folder)):
                    if f.endswith(".md"):
                        doc = parse(os.path.join(folder, f), key)
                        self.docs.setdefault(key, []).append(doc)
                        for s in doc["sections"]:
                            terms = Counter(words(s["text"]) + 2 * words(s["title"]) + words(doc["title"]))
                            self.units.append((doc, s, terms, sum(terms.values())))
        self.df = Counter()
        for _, _, terms, _ in self.units:
            self.df.update(terms.keys())
        self.avg = sum(u[3] for u in self.units) / len(self.units) if self.units else 1.0

    def search(self, community, query, k=4, doc_type=None):
        q = words(query)
        if not q:
            return []
        n = len(self.units)
        scored = []
        for doc, s, terms, length in self.units:
            if doc["community"] != community or (doc_type and doc["type"] != doc_type):
                continue
            score = 0.0
            for w in set(q):
                tf = terms.get(w)
                if not tf:
                    continue
                idf = math.log(1 + (n - self.df[w] + 0.5) / (self.df[w] + 0.5))
                score += idf * tf * (self.K1 + 1) / (tf + self.K1 * (1 - self.B + self.B * length / self.avg))
            if score > 0:
                scored.append((score, doc, s))
        scored.sort(key=lambda x: -x[0])
        return [{"cite": cite(d, s), "document": d["title"], "slug": d["slug"], "section": s["heading"],
                 "score": round(sc, 2), "text": s["text"]} for sc, d, s in scored[:k]]

    def find_doc(self, community, name):
        """A document by its slug, its title or its kind, as speech might say it."""
        docs = self.docs.get(community, [])
        n = re.sub(r"[^a-z0-9]+", " ", (name or "").lower()).strip()
        n = re.sub(r"^(the|our|my) ", "", n)
        if not n:
            return None
        flat = lambda x: re.sub(r"[^a-z0-9]", "", x.lower())
        newest = sorted(docs, key=lambda d: d.get("adopted", ""), reverse=True)   # "the minutes": the latest
        for d in newest:
            if flat(n) in (flat(d["slug"]), flat(d["title"]), flat(d["type"]), flat(short_name(d))):
                return d
        hits = [d for d in docs if all(w in (d["title"] + " " + d["slug"] + " " + d["type"]).lower() for w in n.split())]
        return hits[0] if hits else None

    def section(self, doc, number):
        want = (number or "").strip().lower().rstrip(".")
        for s in doc["sections"]:
            if s["number"].lower() == want or s["heading"].lower() == want or s["title"].lower() == want:
                return s
        return None


_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        _LIB = Library()
    return _LIB
