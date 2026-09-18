# Erzeugt aus _make_SD.bat und 'SternFA SD - latest List.xlsx' zwei Batches:
#   _make_SD.bat               SD-Image auf F:, Dateien als NNN_<Kurzname>.bin
#   _make_roms_FA_Control.bat  dieselben Dateien nach FA_Control\ (ohne GAP-Fueller)
# NNN = Slotnummer = Position der Zeile unter den ROM- und GAP-Zeilen.
# Gleiche Namen auf SD und in FA_Control: ein ROM laesst sich 1:1 von der SD in
# FA Control hochladen (FA Control wertet nur "NNN_" und ".bin" aus).
# Die Namen sind kurz gehalten (<= 13 Zeichen), weil das FAT16-Hauptverzeichnis
# der SD nur 512 Eintraege hat; das Skript rechnet das nach und bricht sonst ab.
#
# _make_SD.bat ist zugleich Quelle und Ziel. Gelesen werden ROM-Zeilen in beiden
# Formen ("... & add_crc16_move <Datei> F:" und "... & call :put NNN <Datei> <Name>")
# sowie GAP-Zeilen ("copy /b 64K F:GAPn"). Kopf, Fuss und Unterprogramm werden neu
# geschrieben. Ein neues ROM also einfach als "... & add_crc16_move <Datei> F:"
# an der richtigen Stelle einfuegen und dieses Skript laufen lassen.
#
# Alle Dateien liegen im SternFA-ROM-Ordner N:\Projekte\FPGA Stern\roms (SRC_DIR,
# ueberschreibbar per Umgebungsvariable STERNFA_ROMS); die Batches arbeiten relativ zu
# sich selbst (%~dp0) und muessen dort bleiben.
#
# Aufruf: python gen_sternfa_roms.py
import os, re, sys, zipfile
from xml.sax.saxutils import unescape

SRC_DIR = os.environ.get("STERNFA_ROMS", r"N:\Projekte\FPGA Stern\roms")
SD = os.path.join(SRC_DIR, "_make_SD.bat")
XLS = os.path.join(SRC_DIR, "SternFA SD - latest List.xlsx")
FA = os.path.join(SRC_DIR, "_make_roms_FA_Control.bat")
PDF = "SternFA SD - latest List.pdf"

lines = open(SD, "rb").read().decode("cp850").splitlines()
ver = re.search(r"SD image (v\d+)", "\n".join(lines)).group(1)

# --- Slots aus _make_SD.bat ---
RE_OLD = re.compile(r"add_crc16_move (\S+) F:")
RE_PUT = re.compile(r"call :put \d{3} (\S+) \S+")
RE_GAP = re.compile(r"F:GAP\d+")
slots = []   # (art, text, datei): art "rom" -> text = alles vor dem Aufruf; "gap" -> ganze Zeile
for line in lines:
    m = RE_OLD.search(line) or RE_PUT.search(line)
    if m:
        slots.append(("rom", line[:m.start()], m.group(1)))
    elif RE_GAP.search(line):
        slots.append(("gap", line.strip(), None))

# --- Spieleliste ohne openpyxl lesen ---
z = zipfile.ZipFile(XLS)
ss = []
x = z.read("xl/sharedStrings.xml").decode("utf-8")
for si in re.findall(r"<si>(.*?)</si>", x, re.S):
    ss.append(unescape("".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))))

def sheet_rows(name):
    rows = []
    for row in re.findall(r"<row[^>]*>(.*?)</row>", z.read(name).decode("utf-8"), re.S):
        d = {}
        for c in re.finditer(r'<c r="([A-Z]+)\d+"([^>]*?)(?:/>|>(.*?)</c>)', row, re.S):
            v = re.search(r"<v>(.*?)</v>", c.group(3) or "")
            v = v.group(1) if v else ""
            if 't="s"' in c.group(2) and v:
                v = ss[int(v)]
            d[c.group(1)] = v.strip()
        rows.append(d)
    return rows

games = None
for n in sorted(k for k in z.namelist() if k.startswith("xl/worksheets/sheet")):
    rows = sheet_rows(n)
    if rows and ver in rows[0].get("B", ""):
        print("Liste:", n, rows[0]["B"])
        games = {int(r["A"]): (r.get("J", ""), r.get("K", "")) for r in rows if r.get("A", "").isdigit()}
if games is None:
    sys.exit("kein Blatt fuer " + ver)

# Kurzname, damit das FAT16-Hauptverzeichnis der SD (512 Eintraege) reicht:
# hoechstens 13 Zeichen = 2 Eintraege je Datei. "NNN_" + 5 Zeichen Spiel, bei einer
# Variante "NNN_" + 3 Zeichen Spiel + "_" + ein Buchstabe (siehe LEGENDE).
LEGENDE = [
    ("F", "Freeplay"),
    ("S", "Special"),
    ("7", "7digit mod"),
    ("V", "V32"),
    ("O", "Okaegi version"),
    ("2", "2 player"),
    ("4", "4 player"),
    ("T", "2 player freeplay"),
    ("Q", "4 player freeplay"),
]
OHNE_VARIANTE = {"", "stern", "stern mpu-200", "bally", "bell games"}

def variante(game, remark):
    k = " ".join(remark.lower().split())
    four = "4 player" in k or "4player" in game.lower().replace(" ", "")
    two = "2 player" in k
    free = "freeplay" in k
    if "special" in k:
        return "S"
    if "7digit" in k:
        return "7"
    if "v32" in k:
        return "V"
    if "okaegi" in k:
        return "O"
    if free and two:
        return "T"
    if free and four:
        return "Q"
    if two:
        return "2"
    if four:
        return "4"
    if free:
        return "F"
    if k not in OHNE_VARIANTE:
        raise ValueError(f"Remark '{remark}' hat keinen Buchstaben - LEGENDE ergaenzen")
    return ""

def kurzname(n, game, remark):
    g = re.sub(r"(?i)4\s*player", "", game)
    g = re.sub(r"[^A-Za-z0-9]", "", g)
    v = variante(game, remark)
    return f"{n}_{g[:3]}_{v}.bin" if v else f"{n}_{g[:5]}.bin"

def fat_eintraege(name):
    """Verzeichniseintraege, die Windows auf FAT fuer diesen Namen anlegt."""
    b, _, e = name.partition(".")
    if len(b) <= 8 and len(e) <= 3 and re.fullmatch(r"[A-Za-z0-9_-]*", b + e) \
            and (b == b.upper() or b == b.lower()) and (e == e.upper() or e == e.lower()):
        return 1
    return -(-len(name) // 13) + 1

FAT_ROOT = 512

def key(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())

# --- Zeilen fuer beide Batches ---
sd_body, fa_body, names, errors, roms = [], [], set(), [], 0
for i, (art, text, datei) in enumerate(slots):
    n = f"{i:03d}"
    game, remark = games.get(i, ("", ""))
    if art == "gap":
        if game != "NOT USED":
            errors.append(f"{n}: GAP, Liste sagt '{game}'")
        sd_body.append(text)
        # keine Datei, aber ein cd im selben Befehl muss erhalten bleiben
        rest = [p.strip() for p in text.split("&") if not RE_GAP.search(p)]
        if rest:
            fa_body.append(" & ".join(rest))
        continue
    if not game or game == "NOT USED":
        errors.append(f"{n}: ROM {datei}, Liste sagt '{game}'")
    try:
        fname = kurzname(n, game, remark)
    except ValueError as e:
        errors.append(f"{n}: {e}")
        continue
    if len(fname) > 13 or fname in names:
        errors.append(f"{n}: Name ungueltig/doppelt: {fname}")
    names.add(fname)
    if not datei.isdigit() and key(datei)[:4] != key(game)[:4]:
        print(f"Abweichung {n}: _make_SD '{datei}' <-> Liste '{game}'")
    if i == 0:
        text = re.sub(r"^cd meteor\b", 'cd /d "%~dp0meteor"', text)
    line = (text + f"call :put {n} {datei} {fname}").rstrip()
    sd_body.append(line)
    fa_body.append(line)
    roms += 1

# Platz im FAT16-Hauptverzeichnis der SD: Label + System Volume Information +
# ROMs + GAPs + Markerdatei + PDF
marker = "SternFA_SD_Image_" + ver
belegt = 1 + fat_eintraege("System Volume Information") + sum(fat_eintraege(x) for x in names) \
    + sum(1 for s in slots if s[0] == "gap") + fat_eintraege(marker) + fat_eintraege(PDF)
print(f"SD-Hauptverzeichnis: {belegt} von {FAT_ROOT} Eintraegen")
if belegt > FAT_ROOT:
    errors.append(f"SD-Hauptverzeichnis laeuft ueber: {belegt} > {FAT_ROOT}")

if errors:
    sys.exit("\n".join(errors))

def put(ziel):
    return [
        "",
        "rem %1 = Slotnummer NNN, %2 = zusammengesetzte Datei im aktuellen Ordner, %3 = Zielname",
        ":put",
        'del /q "%WORK%\\*" 2>nul',
        'add_crc16_move %2 "%WORK%/"',
        'move /y "%WORK%\\*" "' + ziel + '%~3" >nul',
        "goto :eof",
    ]

hinweis = [
    "rem ERZEUGT von FA_Control\\names\\tools\\gen_sternfa_roms.py aus _make_SD.bat und 'SternFA SD - latest List.xlsx'",
    "rem Dateiname NNN_<Spiel 5 Zeichen>.bin oder NNN_<Spiel 3 Zeichen>_<Variante>.bin, NNN = Slot",
    "rem Variante: " + ", ".join(f"{b}={t}" for b, t in LEGENDE),
]

sd = ["@echo off"] + hinweis + [
    "rem SD-Image %s auf F:, Reihenfolge = Slotnummer" % ver,
    'set "WORK=%~dp0_work_SD"',
    'if not exist "%WORK%" mkdir "%WORK%"',
] + sd_body + [
    "echo this is SD image %s > F:%s" % (ver, marker),
    'copy "%~dp0' + PDF + '" F:',
    'rmdir "%WORK%"',
    "echo this is SD image %s" % ver,
    "pause",
    "goto :eof",
] + put("F:")

fa = ["@echo off"] + hinweis + [
    "rem dieselben ROM-Dateien wie auf der SD nach FA_Control\\ (ohne GAP-Fueller)",
    'set "DEST=%~dp0FA_Control"',
    'set "WORK=%~dp0FA_Control\\_work"',
    'if not exist "%WORK%" mkdir "%WORK%"',
] + fa_body + [
    'rmdir "%WORK%"',
    "echo FA_Control: %d ROM-Slots erzeugt" % roms,
    "pause",
    "goto :eof",
] + put("%DEST%\\")

for path, content in ((SD, sd), (FA, fa)):
    open(path, "wb").write(("\r\n".join(content) + "\r\n").encode("cp850"))
print("Slots: %d, davon ROMs: %d" % (len(slots), roms))
