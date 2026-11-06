# Erzeugt die FA_Control-Namensdateien fuer GottFA80:
#   names\GottFA80\<NNN>.cfg  (relativ zu diesem Skript: ..\GottFA80)
#
# EIN Ordner fuer alle Gottlieb-System-80-MPUs (GottFA80 SMD, GottFA80S, GottFA80 PLuS):
# die Spieleliste ist in allen Handbuechern gleich nummeriert. Voraussetzung ist, dass
# jede dieser Platinen in fa_control HW_NAME => "GottFA80" meldet (heute nur PLuS v7,
# FPGA System80\FPGA_source\top\SYS80.vhd). Wer 80/80S mit FA-Control ausstattet, MUSS
# denselben Namen nehmen, sonst sucht FA-Control in einem anderen Ordner.
#
# Spielnummer (Opcode 0x08) = die Zahl der Bootanzeige Spieler 2:
#   Game-Select-DIP S1-1..6 (0..63) + 64, wenn Freeplay-DIP S2-1 an ist
#   (SYS80.vhd: byte_to_ascii mit mybyte = "1" & not opt_freeplay & game_select).
# Deshalb entsteht jede Datei zweimal: <n>.cfg und <n+64>.cfg (Freeplay).
#
# Quellen
#   - Spieleliste:  'GottFA80games.xlsx' im SD-Image-Ordner, neuestes Blatt
#                   'GottFA80games_v<nnn>' (Spalte A = Nr, letzte Spalte = Spiel) --
#                   identisch mit GottFA80games_v501.pdf und den Handbuechern.
#   - Namen:        je Spielfeld eine Abschrift aus dem Gottlieb-Handbuch in
#                   tools\gottfa80_src\<GTB>.ini (Lamp/Switch/Solenoid-Tabellen),
#                   Nummern dort SCHON in Handbuch-Nummerierung (s. u.).
#
# Nummerierung (FA-Control = LISY80 = Gottlieb-Handbuch, FPGA_source\VARIANTEN.md):
#   Lampe    0..47 = Treiber Q1..Q48 (74175 Nr. n/4+1, Bit n%4); 0 Game-Over-Relais,
#            1 Tilt-Relais, 2 Muenzsperre. Die Handbuch-Lampen 48..51 sind die
#            INVERTIERTEN Ausgaenge von 44..47 (lisy80_control.c) -- sie haben keine
#            eigene FA-Nummer und werden bei 44..47 mitgenannt.
#   Spule    1..9 = Solenoid 1..9 des Handbuchs
#   Schalter 0..77 = Handbuch-Schalternummer (Strobe*10 + Return), 07 = Testschalter
#   Sound    Handbuch-Soundnummer, soweit dort eine Tabelle steht
#
# Aufruf: python gen_gottfa80_names.py
import configparser
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gottfa80_src")
XLS = os.environ.get("GOTTFA80_GAMES",
                     r"N:\Projekte\FPGA System80\SD image (roms)\GottFA80games.xlsx")
OUT = os.environ.get("FA_NAMES_OUT", os.path.join(HERE, "..", "GottFA80"))
FREEPLAY = 64

# Nr -> (Quelle tools\gottfa80_src\<key>.ini, Anzeigename, Zusatz fuer den Kopf)
# Die Namen der Liste sind teils verkuerzt ("Devils DareSpeech"); hier steht der
# Spielname, wie er auf dem Startbildschirm erscheinen soll. key() der Liste muss mit
# key() des Namens hier beginnen bzw. ihn enthalten -- sonst bricht das Skript ab.
GAMES = {
    0: ("652", "Panthera"),
    1: ("653", "Spiderman"),
    2: ("654", "Circus"),
    3: ("656", "Counterforce"),
    4: ("657", "Star Race"),
    5: ("658", "James Bond (Timed Play)"),
    6: ("658", "James Bond (3/5-Ball)"),
    7: ("659", "Time Line"),
    8: ("661", "Force II"),
    9: ("664", "Pink Panther"),
    10: ("666", "Mars - God of War (Speech)"),
    11: ("666", "Mars - God of War (Sound only)"),
    12: ("667", "Volcano (Speech, Rev. 4)"),
    13: ("667", "Volcano (Sound only)"),
    14: ("668", "Black Hole"),
    15: ("668", "Black Hole (Sound only)"),
    16: ("669", "Haunted House"),
    17: ("671", "Eclipse"),
    18: ("670", "Devil's Dare (Speech)"),
    19: ("670", "Devil's Dare (Sound only)"),
    20: ("672", "Rocky"),
    21: ("673", "Spirit"),
    22: ("674", "Punk!"),
    23: ("675", "Striker"),
    24: ("676", "Krull"),
    25: ("677", "Q*Bert's Quest"),
    26: ("680", "Super Orbit"),
    27: ("681", "Royal Flush Deluxe"),
    28: ("682", "Goin' Nuts"),
    29: ("684", "Amazon Hunt"),
    30: ("685", "Rack 'Em Up!"),
    31: ("686", "Ready...Aim...Fire!"),
    32: ("687", "Jacks to Open"),
    33: ("688", "Touchdown"),
    34: ("689", "Alien Star"),
    35: ("691", "The Games"),
    36: ("692", "El Dorado City of Gold"),
    37: ("695", "Ice Fever"),
    38: ("pv810", "Caveman"),
    39: ("pv810", "Caveman (flipprojects)"),
    40: ("694", "Bounty Hunter"),
    41: ("696", "Chicago Cubs Triple Play"),
    42: ("698", "Tag Team"),
    43: ("697", "Rock"),
    44: ("702", "Raven"),
    45: ("697", "Rock Encore"),
    46: ("703", "Hollywood Heat"),
    47: ("705", "Genesis"),
    48: ("707", "Gold Wings"),
    49: ("708", "Monte Carlo"),
    50: ("706", "Spring Break"),
    51: ("709", "Arena"),
    52: ("710", "Victory"),
    53: ("711", "Diamond Lady"),
    54: ("712", "TX Sector"),
    55: ("714", "Robo War"),
    56: ("715", "Excalibur"),
    57: ("717", "Bad Girls"),
    58: ("713", "Big House"),
    59: ("718", "Hot Shots"),
    60: ("719", "Bone Busters Inc."),
    61: ("c103", "Night Moves"),
    62: ("684", "Amazon Hunt II"),
    63: ("684", "Amazon Hunt III"),
}

# Fuer alle Spiele gleich (Hardware, nicht Spiel). Steht im Handbuch etwas anderes
# an derselben Nummer, gewinnt das Handbuch.
COMMON = {
    "lamps": {0: "Game Over Relay", 1: "Tilt Relay", 2: "Coin Lockout"},
    # Kabinett (LISY80-CSVs fuer 80 und 80B gleich, Handbuch Bottom Board/Door)
    "switches": {7: "Test Switch", 17: "Left Coin Switch", 27: "Right Coin Switch",
                 37: "Center Coin Switch", 47: "Replay Button", 57: "Tilt"},
}
DISPLAYS = {
    "80":  ["Credits / Ball (Status)", "Player 1", "Player 2", "Player 3", "Player 4"],
    "80A": ["Credits / Ball (Status)", "Player 1", "Player 2", "Player 3", "Player 4"],
    "80B": ["Status", "Display 1", "Display 2", "Display 3", "Display 4"],
}
RANGES = {"lamps": (0, 51), "coils": (1, 9), "switches": (0, 77), "sounds": (0, 31)}
SECTIONS = ("lamps", "coils", "switches", "sounds")


def key(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


# --- Spieleliste (Gegenprobe der Tabelle GAMES) ---------------------------------------
def read_list():
    z = zipfile.ZipFile(XLS)
    wb = z.read("xl/workbook.xml").decode("utf-8")
    rels = z.read("xl/_rels/workbook.xml.rels").decode("utf-8")
    sheets = re.findall(r'<sheet name="(GottFA80games_v(\d+))"[^>]*r:id="(rId\d+)"', wb)
    name, ver, rid = max(sheets, key=lambda s: int(s[1]))
    target = re.search(r'Id="%s"[^>]*Target="([^"]+)"' % rid, rels).group(1)
    ss = [re.sub(r"<[^>]+>", "", s) for s in
          re.findall(r"<si>(.*?)</si>", z.read("xl/sharedStrings.xml").decode("utf-8"), re.S)]
    x = z.read("xl/" + target.lstrip("/").replace("xl/", "")).decode("utf-8")
    games = {}
    for row in re.findall(r"<row[^>]*>(.*?)</row>", x, re.S):
        cells = {}
        for col, attrs, body in re.findall(r'<c r="([A-Z]+)\d+"([^>]*?)(?:/>|>(.*?)</c>)', row, re.S):
            v = re.search(r"<v>(.*?)</v>", body or "")
            if not v:
                continue
            v = v.group(1)
            cells[col] = ss[int(v)] if 't="s"' in attrs else v
        if cells.get("A", "").isdigit() and len(cells) > 1:
            last = cells[max(cells, key=lambda c: (len(c), c))]
            games[int(cells["A"])] = last.replace("&amp;", "&").strip()
    return "v" + ver, games


# --- Quelle je Spielfeld ------------------------------------------------------------------
def read_src(k):
    fn = os.path.join(SRC, k + ".ini")
    if not os.path.exists(fn):
        return None
    head = []
    with open(fn, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            head.append(line.rstrip("\n"))
    cp = configparser.ConfigParser(interpolation=None, comment_prefixes=("#", ";"),
                                   inline_comment_prefixes=None, strict=True)
    cp.optionxform = str
    cp.read(fn, encoding="utf-8")
    meta = dict(cp["meta"]) if cp.has_section("meta") else {}
    data = {}
    for sec in SECTIONS:
        d = {}
        if cp.has_section(sec):
            lo, hi = RANGES[sec]
            for n, v in cp[sec].items():
                n = int(n)
                assert lo <= n <= hi, (k, sec, n)
                if sec == "switches":
                    assert n % 10 <= 7, (k, sec, n)
                v = v.strip()
                assert v and "=" not in v, (k, sec, n, v)
                d[n] = v
        data[sec] = d
    assert meta.get("system") in DISPLAYS, (k, "meta.system")
    return meta, head, data


def fold_lamps(lamps):
    """48..51 (invertierte Ausgaenge) an 44..47 anhaengen."""
    out = {n: v for n, v in lamps.items() if n < 48}
    for inv in range(48, 52):
        if inv in lamps:
            base = inv - 4
            out[base] = "%s / off: %s" % (out.get(base, "(unused)"), lamps[inv])
    return out


def build(nr, gnr, name, k, src, ver):
    lines = [
        "# FA_Control naming file -- %s on GottFA80" % name,
        "#",
        "# ERZEUGT von names/tools/gen_gottfa80_names.py -- nicht von Hand aendern.",
        "# GottFA80 game %d (S1 game select), SD image list %s%s" % (
            gnr, ver, ", FREEPLAY on (S2-1, number + 64)" if nr >= FREEPLAY else ""),
        "# Gilt fuer GottFA80, GottFA80S und GottFA80 PLuS (HW_NAME 'GottFA80').",
    ]
    if src is None:
        lines += ["# Keine Abschrift vorhanden -- nur Name und Systemteile."]
        meta, data = {"system": "80A" if gnr < 40 else "80B"}, {s: {} for s in SECTIONS}
    else:
        meta, head, data = src
        lines += ["#"] + [h for h in head]
    lines += [
        "# Nummern = Gottlieb-Handbuch = LISY80: Lampe 0..47 (48..51 = invertiert 44..47),",
        "# Spule 1..9, Schalter Strobe*10+Return. See names/example.cfg for the format.",
        "# Kein Kommentar hinter dem Wert.",
        "",
        "[game]",
        "name=%s" % name,
    ]
    for sec in SECTIONS:
        d = dict(COMMON.get(sec, {}))
        d.update(fold_lamps(data[sec]) if sec == "lamps" else data[sec])
        if not d:
            continue
        lines += ["", "[%s]" % sec] + ["%d=%s" % (n, d[n]) for n in sorted(d)]
    lines += ["", "[displays]",
              "# LISY-Konvention: 0 = Status, 1..4 = Spieler 1..4."]
    lines += ["%d=%s" % (i, v) for i, v in enumerate(DISPLAYS[meta["system"]])]
    txt = "\n".join(lines) + "\n"
    assert len(txt.encode("utf-8")) <= 32 * 1024, nr
    return txt


def main():
    ver, listed = read_list()
    assert sorted(listed) == list(range(64)), sorted(listed)
    for nr, (k, name) in GAMES.items():
        a, b = key(listed[nr]), key(name)
        assert a[:6] == b[:6] or a in b or b in a, (nr, listed[nr], name)
    os.makedirs(OUT, exist_ok=True)
    cache, missing = {}, set()
    for gnr in range(64):
        k, name = GAMES[gnr]
        if k not in cache:
            cache[k] = read_src(k)
            if cache[k] is None:
                missing.add(k)
        for nr in (gnr, gnr + FREEPLAY):
            with open(os.path.join(OUT, "%03d.cfg" % nr), "w", encoding="utf-8", newline="\n") as f:
                f.write(build(nr, gnr, name, k, cache[k], ver))
    print("%d Dateien nach %s (Liste %s)" % (2 * len(GAMES), os.path.normpath(OUT), ver))
    if missing:
        print("ohne Abschrift:", ", ".join(sorted(missing)))


if __name__ == "__main__":
    sys.exit(main())
