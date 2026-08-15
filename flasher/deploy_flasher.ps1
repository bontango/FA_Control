#Requires -Version 5.1
# deploy_flasher.ps1
#
# Wandelt ..\docs\USB_FLASH.md per Pandoc in USB_FLASH.html um und laedt die
# Installerseite samt Doku, Logo und .htaccess per SFTP (WinSCP) auf lisy.dev.
#
# Das ist bewusst getrennt von build_and_deploy_full.ps1: dort geht es um das
# Flash-Paket und der Build dauert Minuten, hier um eine Handvoll statischer
# Dateien. Ein Tippfehler in der Anleitung soll keinen Rebuild ausloesen.
#
# Voraussetzungen:
#   - Pandoc installiert und im PATH (https://pandoc.org)
#   - WinSCP installiert (wird automatisch gefunden)
#   - .env in diesem Ordner oder im Projektwurzelverzeichnis (..\), mit SFTP_PATH_WEB
#
# Verwendung:
#   .\deploy_flasher.ps1

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# -- Hilfsfunktionen ----------------------------------------------------------
function Info([string]$msg)    { Write-Host "[INFO]  $msg" -ForegroundColor Green }
function Warn([string]$msg)    { Write-Host "[WARN]  $msg" -ForegroundColor Yellow }
function Err([string]$msg)     { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function StepMsg([string]$msg) { Write-Host "`n== $msg ==" -ForegroundColor Cyan }

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
$envFile = $null
if (Test-Path ".env") {
    $envFile = ".env"
} elseif (Test-Path "..\.env") {
    $envFile = "..\.env"
}
if (-not $envFile) {
    Err ".env nicht gefunden (weder hier noch im Projektwurzelverzeichnis ..\)."
    Err "Bitte ..\.env.example nach ..\.env kopieren und mit echten SFTP-Daten befuellen."
    exit 1
}
Info ".env geladen aus: $envFile"

$envVars = @{}
Get-Content $envFile | Where-Object { $_ -match '^\s*[^#\s].+=.' } | ForEach-Object {
    $parts = $_ -split "=", 2
    $envVars[$parts[0].Trim()] = $parts[1].Trim()
}

foreach ($var in @("SFTP_HOST", "SFTP_USER", "SFTP_PATH_WEB")) {
    if (-not $envVars.ContainsKey($var) -or [string]::IsNullOrEmpty($envVars[$var])) {
        Err "Variable '$var' fehlt oder ist leer in $envFile"
        exit 1
    }
}

$SFTP_HOST     = $envVars["SFTP_HOST"]
$SFTP_USER     = $envVars["SFTP_USER"]
$SFTP_PATH_WEB = $envVars["SFTP_PATH_WEB"]

# -- SFTP-Passwort abfragen ---------------------------------------------------
$securePass = Read-Host "SFTP-Passwort fuer ${SFTP_USER}@${SFTP_HOST} (Enter = nur lokale Konvertierung)" -AsSecureString
$bstr       = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePass)
$SFTP_PASS  = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
[System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) | Out-Null

if ([string]::IsNullOrEmpty($SFTP_PASS)) {
    Warn "Kein Passwort eingegeben - SFTP-Upload wird uebersprungen."
    $SFTP_ENABLED = $false
} else {
    $SFTP_ENABLED = $true
}

# -- Pandoc pruefen -----------------------------------------------------------
StepMsg "Pandoc-Konvertierung"

if (-not (Get-Command pandoc -ErrorAction SilentlyContinue)) {
    Err "pandoc nicht im PATH gefunden. Bitte Pandoc installieren: https://pandoc.org/installing.html"
    exit 1
}
Info "Pandoc gefunden: $(pandoc --version | Select-Object -First 1)"

$mdSrc  = "..\docs\USB_FLASH.md"
$htmlOut = "USB_FLASH.html"
if (-not (Test-Path $mdSrc)) {
    Err "Quelldatei nicht gefunden: $mdSrc"
    exit 1
}
Info "Konvertiere $mdSrc -> $htmlOut ..."
pandoc -s -f markdown+gfm_auto_identifiers --metadata title="FA_Control - USB Full Installation" -o $htmlOut $mdSrc
if ($LASTEXITCODE -ne 0) {
    Err "Pandoc fehlgeschlagen (exit $LASTEXITCODE)"
    exit 1
}
Info "OK: $htmlOut erstellt."

# -- SFTP-Upload per WinSCP ---------------------------------------------------
$uploadFiles = @(
    "FA_Control_flasher.html",
    "USB_FLASH.html",
    "logo.png",
    ".htaccess"
)

if ($SFTP_ENABLED) {
    StepMsg "SFTP-Upload"

    foreach ($f in $uploadFiles) {
        if (-not (Test-Path $f)) {
            Err "Datei nicht gefunden: $f"
            exit 1
        }
    }

    $putCommands = ($uploadFiles | ForEach-Object {
        $localPath = (Resolve-Path $_).Path
        "put `"$localPath`" `"${SFTP_PATH_WEB}/$_`""
    }) -join "`n"

    $tmpScript = [System.IO.Path]::GetTempFileName()
    @"
open sftp://${SFTP_USER}@${SFTP_HOST}/ -password="$SFTP_PASS" -hostkey=*
$putCommands
exit
"@ | Set-Content $tmpScript -Encoding UTF8

    try {
        Info "Lade $($uploadFiles.Count) Dateien hoch nach sftp://${SFTP_HOST}${SFTP_PATH_WEB}/ ..."
        & $winscpExe /ini=nul /script=$tmpScript
        if ($LASTEXITCODE -ne 0) { throw "WinSCP exit $LASTEXITCODE" }
        Info "Upload erfolgreich abgeschlossen."
    } catch {
        Err "SFTP-Upload fehlgeschlagen: $_"
        exit 1
    } finally {
        Remove-Item $tmpScript -ErrorAction SilentlyContinue
    }
}

# -- Fertig -------------------------------------------------------------------
Write-Host ""
Write-Host "==================================================" -ForegroundColor Green
Info "Installerseite fertig!"
foreach ($f in $uploadFiles) { Info "  $f" }
if ($SFTP_ENABLED) {
    Info "SFTP-Ziel: sftp://${SFTP_HOST}${SFTP_PATH_WEB}/"
    Info "Die Binaries kommen aus .\..\build_and_deploy_full.ps1 (Ordner esptool\)."
} else {
    Warn "SFTP-Upload wurde uebersprungen (kein Passwort eingegeben)."
}
Write-Host "==================================================" -ForegroundColor Green
