# Erzeugt die FA_Control-Namensdateien fuer alle SternFA-Slots:
#   names\SternFA\<NNN>.cfg  (relativ zu diesem Skript: ..\SternFA)
#
# Quellen
#   - Spieleliste:  'SternFA SD - latest List.xlsx', Blatt der Image-Version aus
#                   _make_SD.bat (Spalte A = Slot, J = Spiel, K = Remark); beide liegen
#                   im SternFA-Projekt unter N:\Projekte\FPGA Stern\roms (SRC_DIR,
#                   ueberschreibbar per Umgebungsvariable STERNFA_ROMS)
#   - Namen:        PinWiki, eine Seite je Spiel (Lamp Chart, Solenoids, Switch Matrix)
#   - Umrechnung:   zwei feste Tabellen aus den Schaltplaenen der Treiberplatinen,
#                   gleich fuer alle Bally-/Stern-Spiele dieser Bauart (s. unten)
#
# Nummerierung wie SternFA/LISY35 (FPGA_source\rtl\fa_control\fa_io_bally.vhd):
#   Lampe   = 4514-Adresse + 15 * Datenbit (U1..U4)       0..59
#   Spule   = 74154-Wert + 1 (1..15), Dauerausgaenge PB4..PB7 = 16..19
#   Schalter= Strobe * 8 + Return                         0..39  (= Handbuch-Nr. - 1)
#
# PinWiki-Seiten werden in %TEMP%\pinwiki_cache zwischengespeichert; --refresh laedt neu.
# Aufruf: python gen_sternfa_names.py [--refresh]
import html
import os
import re
import sys
import time
import urllib.request
import zipfile
from xml.sax.saxutils import unescape

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.environ.get("STERNFA_ROMS", r"N:\Projekte\FPGA Stern\roms")
SD = os.path.join(SRC_DIR, "_make_SD.bat")
XLS = os.path.join(SRC_DIR, "SternFA SD - latest List.xlsx")
OUT = os.environ.get("FA_NAMES_OUT", os.path.join(HERE, "..", "SternFA"))
CACHE = os.path.join(os.environ.get("TEMP", HERE), "pinwiki_cache")
WIKI = "https://www.pinwiki.com/wiki/index.php/"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36"

# --- Lampentreiber AS-2518-23 / LDU-100: 4514-Ausgang 0..14 von U1..U4 -> SCR Qn -----
# Aus dem Schaltplan (N:\Flipper\Bally\bally_lampdriver.pdf, SCR-Nr = Widerstands-Nr).
# Die Zuordnung SCR -> U bestaetigt PinWiki "Lamp Transistor Mapping for Classic Stern
# Games" (bis auf Q15, das dort faelschlich bei U1 steht). Gegenprobe Seawitch-ROM:
# 12 kommentierte Lampen landen auf dem passenden PinWiki-Namen.
LAMP_U = [
    [14, 12, 13, 8, 9, 10, 11, 4, 1, 2, 3, 7, 16, 5, 6],
    [29, 27, 28, 35, 34, 22, 26, 25, 24, 17, 23, 21, 15, 18, 19],
    [36, 38, 44, 49, 48, 37, 32, 20, 42, 41, 40, 39, 33, 30, 31],
    [57, 50, 51, 54, 55, 60, 59, 58, 56, 46, 52, 53, 47, 43, 45],
]
LAMP_OF_SCR = {q: a + 15 * d for d, qs in enumerate(LAMP_U) for a, q in enumerate(qs)}
assert sorted(LAMP_OF_SCR) == list(range(1, 61))

# --- Spulentreiber AS-2518-16 / SDU-100: Treiber Qn -> FA-Spule ------------------------
# Aus dem Schaltplan (N:\Flipper\Bally\Bally_solenoiddriverregulator.pdf): 74154-Ausgang
# 0..14 -> Q2 Q1 Q5 Q6 Q7 Q3 Q4 Q8 Q13 Q14 Q9 Q10 Q12 Q11 Q16; Dauerausgaenge
# PB4 (Cont 2) -> Q17, PB5 (Cont 4, Coin Lockout) -> Q19, PB6 (Cont 1, Flipper) -> Q15,
# PB7 (Cont 3) -> Q18. Gegenprobe Seawitch-ROM: alle 10 benutzten Spulen passen.
# ACHTUNG: "Sol. No" auf PinWiki ist KEINE verlaessliche Spulennummer (bei Seawitch = Qn,
# bei den Bally-Seiten wechselnd) -- gelesen wird deshalb nur die Treiber-Spalte.
SOL_MOM = [2, 1, 5, 6, 7, 3, 4, 8, 13, 14, 9, 10, 12, 11, 16]
COIL_OF_Q = {q: i + 1 for i, q in enumerate(SOL_MOM)}
COIL_OF_Q.update({17: 16, 19: 17, 15: 18, 18: 19})

# --- Spielname aus der Liste -> PinWiki-Seite ---------------------------------------------
PAGE_ALIAS = {
    "6millionman": "The_Six_Million_Dollar_Man",
    "harlemglobe": "Harlem_Globetrotters",
    "nitroground": "Nitro_Groundshaker",
    "strikesspares": "Strikes_%26_Spares",
    "xandos": "X%27s_%26_O%27s",
    "mrmrspacman": "Mr._%26_Mrs._Pacman",
    "8balldeluxe": "Eight_Ball_Deluxe",
    "eightball": "Eight_Ball",
    "playboy": "Bally_Playboy",
    "kiss": "Bally_Kiss",
    "lostworld": "Bally_Lost_World",
    "startrek": "Bally_Star_Trek",
    "rollingstones": "Bally_Rolling_Stones",
    "babypacman": "Baby_Pac-Man",
    "fireballclassic": "Fireball_Classic",
    "speakeasy4player": "Speakeasy",
}
# Spiele, fuer die es keine PinWiki-Seite gibt (Bell Games) oder deren Platine anders
# verdrahtet ist -- dafuer entsteht nur [game] und [displays].
NO_PAGE = {"tigerrag", "saturn2", "worlddefender"}
# Goldball und Grand Slam haben die Kombiplatine AS-2518-147 (Lampen + Spulen auf einer
# Platine, LISY35: AS_2518_147_LAMP_COMBO) -- die Tabellen oben gelten dort nicht.
COMBO_BOARD = {"goldball", "grandslam"}
# Baby Pac-Man: Treiber heissen dort Q31..Q40, eigene Platine -- Tabellen gelten nicht.
OTHER_BOARD = {"babypacman"}

# Tabellen, die auf PinWiki nachweislich falsch sind und verworfen werden
# (Stargazer: Lampen- und Spulentabelle wortgleich von Seawitch kopiert, 09.2026 geprueft;
# die Schaltermatrix ist echt).
DISCARD = {
    "Stargazer": ("lamps", "coils"),
}

# Ergaenzungen, die nicht auf PinWiki stehen
EXTRA = {
    "Seawitch": {"lamps": {41: "Special Right Outlane"}},   # Seawitch-ROM: $2e "right outlane special"
    # VPX "Star Gazer (Stern 1980) v2.0.0" (PinMAME = FA-Nummer bei 1..15; die
    # Dauerausgaenge zaehlt PinMAME eins hoeher: PinMAME 19 = PB6 = FA 18).
    "Stargazer": {"coils": {3: "Center Drop Target Reset", 4: "Right Drop Target Reset",
                            6: "Knocker", 7: "Left Drop Target Reset", 12: "Ball Release",
                            18: "Flipper Enable Relay"}},
}

VARIANT_NAME = [
    ("7digit", "7-digit mod"), ("special w. sirene", "Special, siren patch"),
    ("special", "Special"), ("v32", "V32"), ("okaegi", "Okaegi"),
]


def key(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


# --- Spieleliste ---------------------------------------------------------------------------
def read_games():
    ver = re.search(r"SD image (v\d+)", open(SD, encoding="cp850").read()).group(1)
    z = zipfile.ZipFile(XLS)
    ss = ["".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))
          for si in re.findall(r"<si>(.*?)</si>", z.read("xl/sharedStrings.xml").decode("utf-8"), re.S)]
    ss = [unescape(s) for s in ss]
    for n in sorted(k for k in z.namelist() if k.startswith("xl/worksheets/sheet")):
        rows = []
        for row in re.findall(r"<row[^>]*>(.*?)</row>", z.read(n).decode("utf-8"), re.S):
            d = {}
            for c in re.finditer(r'<c r="([A-Z]+)\d+"([^>]*?)(?:/>|>(.*?)</c>)', row, re.S):
                v = re.search(r"<v>(.*?)</v>", c.group(3) or "")
                v = v.group(1) if v else ""
                if 't="s"' in c.group(2) and v:
                    v = ss[int(v)]
                d[c.group(1)] = v.strip()
            rows.append(d)
        if rows and ver in rows[0].get("B", ""):
            games = {}
            for r in rows:
                if r.get("A", "").isdigit() and r.get("J") and r["J"] != "NOT USED":
                    games[int(r["A"])] = (r["J"], r.get("K", ""))
            return ver, games
    sys.exit("kein Blatt fuer " + ver)


# --- PinWiki -------------------------------------------------------------------------------
def fetch(page, refresh):
    os.makedirs(CACHE, exist_ok=True)
    fn = os.path.join(CACHE, re.sub(r"[^A-Za-z0-9_.-]", "_", page) + ".html")
    if refresh or not os.path.exists(fn):
        req = urllib.request.Request(WIKI + page, headers={"User-Agent": UA, "Accept": "text/html"})
        try:
            data = urllib.request.urlopen(req, timeout=30).read()
        except Exception as e:
            print(f"  {page}: {e}")
            return None
        open(fn, "wb").write(data)
        time.sleep(1)   # das Wiki schonen
    return open(fn, encoding="utf-8", errors="replace").read()


def cells(tr):
    return [" ".join(html.unescape(re.sub(r"<[^>]+>", " ", c)).split())
            for c in re.findall(r"<t[hd][^>]*>(.*?)</t[hd]>", tr, re.S)]


def parse(page_html):
    body = page_html[page_html.find("mw-content-text"):]
    title = re.search(r"<title>(.*?) - PinWiki</title>", page_html)
    res = {"title": html.unescape(title.group(1)) if title else "", "lamps": {}, "coils": {},
           "switches": {}, "aux_lamps": 0, "notes": []}
    for tab in re.findall(r"<table.*?</table>", body, re.S):
        rows = [cells(tr) for tr in re.findall(r"<tr.*?</tr>", tab, re.S)]
        if not rows:
            continue
        hdr = [h.lower() for h in rows[0]]
        if hdr and hdr[0] == "scr" and any("lamp" in h for h in hdr):
            ci = hdr.index("scr")
            ni = next(i for i, h in enumerate(hdr) if "lamp" in h)
            pi = next((i for i, h in enumerate(hdr) if "conn" in h), None)
            for r in rows[1:]:
                if len(r) <= ni:
                    continue
                m = re.fullmatch(r"Q0*(\d+)", r[ci])
                name = r[ni].strip()
                if not m or not name:
                    continue
                q = int(m.group(1))
                conn = r[pi] if pi is not None else ""
                if conn.startswith("*") or not conn.startswith("A5") and conn[:1] == "A" and conn[:2] != "A5":
                    res["aux_lamps"] += 1      # Zusatzplatine: fuer SternFA nicht erreichbar
                    continue
                if q not in LAMP_OF_SCR:
                    res["notes"].append(f"Lampe Q{q} unbekannt: {name}")
                    continue
                n = LAMP_OF_SCR[q]
                old = res["lamps"].get(n)
                res["lamps"][n] = name if not old or old == name else old + " / " + name
        elif any("sol" in h and "no" in h for h in hdr):
            di = 1
            qi = next((i for i, h in enumerate(hdr) if "transistor" in h), None)
            if qi is None:
                res["notes"].append("Spulentabelle ohne Treiberspalte")
                continue
            for r in rows[1:]:
                if len(r) <= qi or not r[0].isdigit():
                    continue
                m = re.fullmatch(r"Q0*(\d+)", r[qi])
                name = r[di].strip()
                if not m or not name or name.lower() in ("n/u", "not used", "--"):
                    continue
                q = int(m.group(1))
                if q not in COIL_OF_Q:
                    res["notes"].append(f"Spule Q{q} unbekannt: {name}")
                    continue
                n = COIL_OF_Q[q]
                old = res["coils"].get(n)
                res["coils"][n] = name if not old or old == name else old + " / " + name
        elif any("strobe" in c.lower() for r in rows[:2] for c in r):
            for r in rows:
                for c in r:
                    m = re.fullmatch(r"(\d{1,2}) (.+)", c)
                    if m and 1 <= int(m.group(1)) <= 40:
                        name = m.group(2).strip()
                        if name.lower().startswith(("not used", "n/u")):
                            continue
                        n = int(m.group(1)) - 1
                        if n in res["switches"] and res["switches"][n] != name:
                            res["notes"].append(f"Schalter {n + 1} doppelt: {res['switches'][n]} / {name}")
                        res["switches"][n] = name
    return res


# --- Ausgabe ---------------------------------------------------------------------------------
def variant(game, remark):
    k = remark.lower()
    parts = []
    for tok, txt in VARIANT_NAME:
        if tok in k:
            parts.append(txt)
            break
    if re.search(r"4 ?player", (game + " " + remark).lower()):
        parts.append("4 player")
    elif "2 player" in k:
        parts.append("2 player")
    if "freeplay" in k:
        parts.append("Freeplay")
    return ", ".join(parts)


def write_cfg(slot, ver, game, remark, page, info):
    title = (info or {}).get("title") or re.sub(r"\s*4Player", "", game).title()
    title = re.sub(r"^Bally ", "", title).replace("Grandslam", "Grand Slam")
    var = variant(game, remark)
    name = title + (f" ({var})" if var else "")
    L = ["# FA_Control naming file -- %s on SternFA" % name,
         "#",
         "# ERZEUGT von names/tools/gen_sternfa_names.py -- nicht von Hand aendern.",
         "# SternFA slot %03d of SD image %s ('SternFA SD - latest List.xlsx': %s / %s)"
         % (slot, ver, game, remark or "-"),
         "#"]
    if page:
        L.append("# Namen: PinWiki %s%s" % (WIKI, page))
    L += ["# Umrechnung: Schaltplaene AS-2518-23 (Lampen) und AS-2518-16 (Spulen), s. Generator.",
          "# PinWiki 'Sol. No' ist KEINE Spulennummer -- gelesen wird die Treiberspalte Qn.",
          "# See names/example.cfg for the format. Kein Kommentar hinter dem Wert.",
          "", "[game]", "name=" + name, ""]
    if info:
        extra = EXTRA.get(title, {})
        for sec in DISCARD.get(title, ()):
            L.append("# [%s] von PinWiki verworfen: dort wortgleich von einem anderen Spiel kopiert." % sec)
        if DISCARD.get(title):
            L.append("")
        for sec, head in (("coils", "# FA-Nummer = Treiber Qn laut AS-2518-16 (1..15 momentan, 16..19 dauernd)"),
                          ("switches", "# FA-Nummer = Strobe * 8 + Return = Handbuch-/PinWiki-Nummer - 1"),
                          ("lamps", "# FA-Nummer = Adresse + 15 * Datenbit (U1..U4) laut AS-2518-23")):
            d = {} if sec in DISCARD.get(title, ()) else dict(info[sec])
            d.update(extra.get(sec, {}))
            if not d:
                continue
            L += ["[%s]" % sec, head]
            if extra.get(sec):
                L.append("# ergaenzt (nicht von PinWiki): %s" % ", ".join(str(k) for k in sorted(extra[sec]))
                         + " -- Quelle s. EXTRA im Generator")
            if sec == "lamps" and info["aux_lamps"]:
                L.append("# %d Lampen der Zusatzplatine fehlen: SternFA treibt nur die Hauptplatine."
                         % info["aux_lamps"])
            for k in sorted(d):
                L.append("%d=%s" % (k, d[k].replace("=", "-")))
            L.append("")
    elif page is None:
        k = key(game)
        why = ("Kombiplatine AS-2518-147, die Tabellen gelten dort nicht" if k in COMBO_BOARD else
               "eigene Treiberplatine (Q31..Q40), die Tabellen gelten dort nicht" if k in OTHER_BOARD else
               "keine PinWiki-Seite")
        L += ["# Keine Belegung: " + why, ""]
    elif not any(info[s] for s in ("lamps", "coils", "switches")):
        L += ["# Keine Belegung: die PinWiki-Seite hat keine Tabellen.", ""]
    L += ["[displays]", "# LISY-Konvention: 0 = Status/Credit, 1..4 = Spieler 1..4.",
          "0=Credits / Ball", "1=Player 1", "2=Player 2", "3=Player 3", "4=Player 4", ""]
    txt = "\n".join(L)
    assert len(txt.encode("utf-8")) < 32 * 1024
    with open(os.path.join(OUT, "%03d.cfg" % slot), "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)
    return name


def main():
    refresh = "--refresh" in sys.argv
    ver, games = read_games()
    os.makedirs(OUT, exist_ok=True)
    links = {}
    idx = fetch("Bally/Stern", refresh)
    for href, txt in re.findall(r'<a href="/wiki/index\.php/([^"#?]+)"[^>]*>(.*?)</a>', idx):
        links.setdefault(key(html.unescape(re.sub(r"<[^>]+>", "", txt))), href)

    parsed, report = {}, []
    for slot in sorted(games):
        game, remark = games[slot]
        k = key(game)
        if k in NO_PAGE or k in COMBO_BOARD or k in OTHER_BOARD:
            page = None
        else:
            page = PAGE_ALIAS.get(k) or links.get(k) or links.get(key(re.sub(r"(?i)4\s*player", "", game)))
        info = None
        if page:
            if page not in parsed:
                h = fetch(page, refresh)
                parsed[page] = parse(h) if h else None
            info = parsed[page]
        name = write_cfg(slot, ver, game, remark, page, info)
        if info:
            t = re.sub(r"\s*\(.*\)$", "", name)
            cnt = [0 if s in DISCARD.get(t, ()) else len(info[s]) for s in ("lamps", "coils", "switches")]
            cnt = [c + len(EXTRA.get(t, {}).get(s, {})) for c, s in zip(cnt, ("lamps", "coils", "switches"))]
            report.append((slot, name, *cnt, info["aux_lamps"], info["notes"]))
        else:
            report.append((slot, name, 0, 0, 0, 0, ["ohne Belegung" + ("" if page is None else ", Seite fehlt")]))

    # Kopien auf PinWiki erkennen: dieselbe Tabelle bei zwei verschiedenen Spielen
    seen = {}
    for page, info in parsed.items():
        if not info:
            continue
        for sec in ("lamps", "coils", "switches"):
            if len(info[sec]) < 5:
                continue
            sig = (sec, tuple(sorted(info[sec].items())))
            if sig in seen and seen[sig] != info["title"]:
                other = seen[sig]
                handled = sec in DISCARD.get(info["title"], ()) or sec in DISCARD.get(other, ())
                print(f"!! {sec} von {info['title']} und {other} sind identisch"
                      + (" (verworfen, s. DISCARD)" if handled else " -- PRUEFEN"))
            seen.setdefault(sig, info["title"])

    print(f"{len(games)} Dateien nach {OUT}")
    print("Slot  Lamp Coil Sw  Aux  Name / Hinweise")
    for slot, name, nl, nc, ns, aux, notes in report:
        print(f"{slot:03d}  {nl:4d} {nc:4d} {ns:3d} {aux:4d}  {name}" + ("  !! " + "; ".join(notes) if notes else ""))


if __name__ == "__main__":
    main()
