#Requires -Version 5.1
# build_and_deploy_full.ps1
#
# Baut FA_Control und legt das VOLLSTAENDIGE Flash-Paket ab -- die vier Dateien,
# die der USB-Installer (flasher\FA_Control_flasher.html) an den ESP32-C3 schreibt:
#   - bootloader.bin         (Bootloader, Offset 0x0)
#   - partition-table.bin    (Partitionstabelle, 0x8000 -- traegt die names-Partition)
#   - ota_data_initial.bin   (OTA-Auswahl, 0xf000)
#   - FA_Control.bin         (Anwendung, 0x20000 = ota_0)
# Dazu eine version.txt, aus der die Installerseite die angebotene Version liest.
#
# Das ist der zweite Release-Weg neben build_and_deploy.ps1: dort geht nur die
# Anwendung nach .../FA_Control/bin/ (fuer OTA), hier das ganze Paket nach
# .../FA_Control/esptool/. Beide wollen bei einem Versionswechsel bedient werden.
#
# Hinweis: N: ist ein UNC-Netzlaufwerk - der Build laeuft daher zwingend
# in einem lokalen Build-Verzeichnis (Standard: C:\Users\bonta\esp\build\fa).
#
# Voraussetzungen:
#   - WinSCP installiert (wird automatisch gefunden)
#   - .env Datei im Projektverzeichnis mit SFTP_PATH_FULL (siehe .env.example)
#
# Verwendung:
#   .\build_and_deploy_full.ps1

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# -- Hilfsfunktionen ----------------------------------------------------------
function Info([string]$msg)    { Write-Host "[INFO]  $msg" -ForegroundColor Green }
function Warn([string]$msg)    { Write-Host "[WARN]  $msg" -ForegroundColor Yellow }
function Err([string]$msg)     { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function StepMsg([string]$msg) { Write-Host "`n== $msg ==" -ForegroundColor Cyan }

function Write-FileUtf8NoBom([string]$path, [string]$content) {
    # Ohne diesen Umweg schreibt Set-Content -Encoding UTF8 eine BOM, und die
    # Installerseite haette ein unsichtbares \uFEFF vor der Versionsnummer.
    $enc = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($path, $content, $enc)
}

# -- WinSCP suchen ------------------------------------------------------------
$winscpPaths = @(
    "C:\Program Files (x86)\WinSCP\WinSCP.com",
    "C:\Program Files\WinSCP\WinSCP.com"
)
$winscpExe = $null
foreach ($p in $winscpPaths) {
    if (Test-Path $p) { $winscpExe = $p; break }
}
if (-not $winscpExe) {
    $winscpExe = Get-Command WinSCP.com -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if (-not $winscpExe) {
    Err "WinSCP.com nicht gefunden. Bitte WinSCP installieren: https://winscp.net"
    exit 1
}
Info "WinSCP gefunden: $winscpExe"

# -- .env laden ---------------------------------------------------------------
if (-not (Test-Path ".env")) {
    Err ".env nicht gefunden."
    Err "Bitte .env.example nach .env kopieren und mit echten SFTP-Daten befuellen."
    exit 1
}
$envVars = @{}
Get-Content ".env" | Where-Object { $_ -match '^\s*[^#\s].+=.' } | ForEach-Object {
    $parts = $_ -split "=", 2
    $envVars[$parts[0].Trim()] = $parts[1].Trim()
}

foreach ($var in @("SFTP_HOST", "SFTP_USER", "SFTP_PATH_FULL")) {
    if (-not $envVars.ContainsKey($var) -or [string]::IsNullOrEmpty($envVars[$var])) {
        Err "Variable '$var' fehlt oder ist leer in .env"
        exit 1
    }
}

$SFTP_HOST      = $envVars["SFTP_HOST"]
$SFTP_USER      = $envVars["SFTP_USER"]
$SFTP_PATH_FULL = $envVars["SFTP_PATH_FULL"]
if ($envVars["LOCAL_RELEASES_DIR"]) {
    $LOCAL_RELEASES_DIR = $envVars["LOCAL_RELEASES_DIR"] + "\full"
} else {
    $LOCAL_RELEASES_DIR = "releases\full"
}
if ($envVars["BUILD_DIR"]) {
    $BUILD_DIR = $envVars["BUILD_DIR"]
} else {
    $BUILD_DIR = "C:\Users\bonta\esp\build\fa"
}

# -- SFTP-Passwort abfragen ---------------------------------------------------
$securePass = Read-Host "SFTP-Passwort fuer ${SFTP_USER}@${SFTP_HOST} (Enter = nur lokal kopieren)" -AsSecureString
$bstr       = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePass)
$SFTP_PASS  = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
[System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) | Out-Null

if ([string]::IsNullOrEmpty($SFTP_PASS)) {
    Warn "Kein Passwort eingegeben - SFTP-Upload wird uebersprungen."
    $SFTP_ENABLED = $false
} else {
    $SFTP_ENABLED = $true
}

# -- ESP-IDF Umgebung ---------------------------------------------------------
# Welche Python-Umgebung ESP-IDF benutzt, entscheidet IDF_PYTHON_ENV_PATH. Ohne die
# Variable bildet idf_tools.py den venv-Namen aus der Version des Interpreters, den
# export.ps1 gerade aufruft -- und das ist schlicht das erste 'python' im PATH, hier
# der Microsoft-Store-Alias auf 3.14. So entstand ein zweites venv, waehrend das
# Build-Verzeichnis mit einem anderen konfiguriert war; CMake bricht dann ab mit
# "... is currently active while the project was configured with ...".
#
# Der Pin steht deshalb VOR der idf.py-Pruefung: laeuft das Script aus einem bereits
# exportierten Terminal, wuerde er sonst uebersprungen und die dort aktive Umgebung
# gewinnt still.
$IdfPythonEnv = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env"

if ($env:IDF_PYTHON_ENV_PATH -and $env:IDF_PYTHON_ENV_PATH -ne $IdfPythonEnv) {
    Err "Aktive ESP-IDF-Python-Umgebung passt nicht:"
    Err "  aktiv:    $env:IDF_PYTHON_ENV_PATH"
    Err "  erwartet: $IdfPythonEnv"
    Err "Bitte eine frische PowerShell oeffnen (dort greift die Benutzervariable)."
    exit 1
}
$env:IDF_PYTHON_ENV_PATH = $IdfPythonEnv

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $IdfExport = "$env:USERPROFILE\esp\v5.5.1\esp-idf\export.ps1"
    if (Test-Path $IdfExport) {
        Info "Lade ESP-IDF Umgebung (venv: $env:IDF_PYTHON_ENV_PATH) ..."
        . $IdfExport
    } else {
        Err "idf.py nicht im PATH und $IdfExport nicht gefunden."
        Err "Bitte ESP-IDF Terminal verwenden oder Pfad anpassen."
        exit 1
    }
}

# -- Version aus version.txt auslesen -----------------------------------------
if (-not (Test-Path "version.txt")) {
    Err "version.txt nicht gefunden."
    exit 1
}
$VERSION = (Get-Content "version.txt" -TotalCount 1).Trim()
if ([string]::IsNullOrEmpty($VERSION)) {
    Err "version.txt ist leer."
    exit 1
}
Info "Version erkannt: $VERSION"

# -- Verzeichnis vorbereiten --------------------------------------------------
New-Item -ItemType Directory -Force -Path $LOCAL_RELEASES_DIR | Out-Null

# -- Build + Copy + SFTP ------------------------------------------------------
try {
    StepMsg "Build FA_Control v$VERSION - Full Flash Package"
    Info "Starte idf.py -B $BUILD_DIR build ..."
    idf.py -B $BUILD_DIR build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build fehlgeschlagen (exit $LASTEXITCODE)" }

    # Quelldateien -- Pfade und Offsets stehen in $BUILD_DIR\flash_args
    $copies = @(
        @{ Src = "$BUILD_DIR\bootloader\bootloader.bin";           Dst = "bootloader.bin" },
        @{ Src = "$BUILD_DIR\partition_table\partition-table.bin"; Dst = "partition-table.bin" },
        @{ Src = "$BUILD_DIR\ota_data_initial.bin";                Dst = "ota_data_initial.bin" },
        @{ Src = "$BUILD_DIR\FA_Control.bin";                      Dst = "FA_Control.bin" }
    )

    Info "Kopiere Binaries nach $LOCAL_RELEASES_DIR ..."
    foreach ($c in $copies) {
        if (-not (Test-Path $c.Src)) { throw "Build-Artefakt fehlt: $($c.Src)" }
        Copy-Item $c.Src "$LOCAL_RELEASES_DIR\$($c.Dst)" -Force
    }

    # Die Installerseite holt sich diese Datei und zeigt die Version an.
    $verFile = Join-Path (Resolve-Path $LOCAL_RELEASES_DIR).Path "version.txt"
    Write-FileUtf8NoBom $verFile "$VERSION`n"
    Info "version.txt geschrieben ($VERSION, ohne BOM)."

    # CORS-Kopf fuer den Fall, dass jemand die Installerseite lokal von der Platte
    # oeffnet: dann ist der Origin 'null' und der Download der Binaries braucht ihn.
    $htaccessSrc = "flasher\esptool.htaccess"
    if (-not (Test-Path $htaccessSrc)) { throw "$htaccessSrc nicht gefunden." }

    if ($SFTP_ENABLED) {
        Info "SFTP-Upload -> sftp://${SFTP_HOST}${SFTP_PATH_FULL}/"

        # WinSCP-Script in temporaere Datei schreiben (Passwort bleibt aus Prozessliste)
        $puts = @()
        foreach ($c in $copies) {
            $local = (Resolve-Path "$LOCAL_RELEASES_DIR\$($c.Dst)").Path
            $puts += "put `"$local`" `"${SFTP_PATH_FULL}/$($c.Dst)`""
        }
        $puts += "put `"$verFile`" `"${SFTP_PATH_FULL}/version.txt`""
        $puts += "put `"$((Resolve-Path $htaccessSrc).Path)`" `"${SFTP_PATH_FULL}/.htaccess`""
        $putCommands = $puts -join "`n"

        $tmpScript = [System.IO.Path]::GetTempFileName()
        @"
open sftp://${SFTP_USER}@${SFTP_HOST}/ -password="$SFTP_PASS" -hostkey=*
$putCommands
exit
"@ | Set-Content $tmpScript -Encoding UTF8

        try {
            & $winscpExe /ini=nul /script=$tmpScript
            if ($LASTEXITCODE -ne 0) { throw "WinSCP exit $LASTEXITCODE" }
        } finally {
            Remove-Item $tmpScript -ErrorAction SilentlyContinue
        }

        Info "OK: Vollpaket erfolgreich hochgeladen."
    }
}
catch {
    Err "Build abgebrochen: $_"
    exit 1
}

# -- Fertig -------------------------------------------------------------------
Write-Host ""
Write-Host "==================================================" -ForegroundColor Green
Info "Build & Deploy (Full) erfolgreich abgeschlossen!"
Info "Lokal gespeichert in: $LOCAL_RELEASES_DIR\"
Info "  bootloader.bin        -> 0x0"
Info "  partition-table.bin   -> 0x8000"
Info "  ota_data_initial.bin  -> 0xf000"
Info "  FA_Control.bin        -> 0x20000"
Info "  version.txt           ($VERSION)"
if ($SFTP_ENABLED) {
    Info "SFTP-Ziel: sftp://${SFTP_HOST}${SFTP_PATH_FULL}/"
    Info "Die Installerseite wird getrennt veroeffentlicht: .\flasher\deploy_flasher.ps1"
} else {
    Warn "SFTP-Upload wurde uebersprungen (kein Passwort eingegeben)."
}
Write-Host "==================================================" -ForegroundColor Green
