#Requires -Version 5.1
# build_and_deploy_full.ps1
#
# Baut pwavplayer (ESP32 WAV-Player fuer Flipper-Soundsysteme) und laedt
# alle 4 Flash-Dateien per SFTP (WinSCP) hoch:
#   - pwavplayer.bin         (Anwendung)
#   - bootloader.bin         (Bootloader)
#   - partition-table.bin    (Partitionstabelle)
#   - ota_data_initial.bin   (OTA-Daten)
#
# Voraussetzungen:
#   - WinSCP installiert (wird automatisch gefunden)
#   - .env Datei im Projektverzeichnis mit SFTP_HOST, SFTP_USER, SFTP_PATH_FULL
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
    $enc = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText((Resolve-Path $path).Path, $content, $enc)
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
    Err "Bitte .env mit SFTP_HOST, SFTP_USER und SFTP_PATH_FULL befuellen."
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
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $IdfExport = "$env:USERPROFILE\esp\v5.5.1\esp-idf\export.ps1"
    if (Test-Path $IdfExport) {
        if (-not $env:IDF_PYTHON_ENV_PATH) {
            $env:IDF_PYTHON_ENV_PATH = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env"
        }
        Info "Lade ESP-IDF Umgebung (venv: $env:IDF_PYTHON_ENV_PATH) ..."
        . $IdfExport
    } else {
        Err "idf.py nicht im PATH und $IdfExport nicht gefunden."
        Err "Bitte ESP-IDF Terminal verwenden oder Pfad anpassen."
        exit 1
    }
}

# -- Verzeichnis vorbereiten --------------------------------------------------
New-Item -ItemType Directory -Force -Path $LOCAL_RELEASES_DIR | Out-Null

# -- Build --------------------------------------------------------------------
try {
    StepMsg "Build pwavplayer - Full Flash Package"

    Info "Starte idf.py build ..."
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build fehlgeschlagen (exit $LASTEXITCODE)" }

    # Quelldateien
    $srcApp       = "build\pwavplayer.bin"
    $srcBoot      = "build\bootloader\bootloader.bin"
    $srcPartition = "build\partition_table\partition-table.bin"
    $srcOta       = "build\ota_data_initial.bin"

    # Zieldateien (lokal)
    $dstApp       = "$LOCAL_RELEASES_DIR\pwavplayer.bin"
    $dstBoot      = "$LOCAL_RELEASES_DIR\bootloader.bin"
    $dstPartition = "$LOCAL_RELEASES_DIR\partition-table.bin"
    $dstOta       = "$LOCAL_RELEASES_DIR\ota_data_initial.bin"

    Info "Kopiere Binaries nach $LOCAL_RELEASES_DIR ..."
    Copy-Item $srcApp       $dstApp       -Force
    Copy-Item $srcBoot      $dstBoot      -Force
    Copy-Item $srcPartition $dstPartition -Force
    Copy-Item $srcOta       $dstOta       -Force
    Info "Alle 4 Dateien lokal gespeichert."

    if ($SFTP_ENABLED) {
        Info "SFTP-Upload -> sftp://${SFTP_HOST}${SFTP_PATH_FULL}/"

        $tmpScript = [System.IO.Path]::GetTempFileName()
        @"
open sftp://${SFTP_USER}@${SFTP_HOST}/ -password="$SFTP_PASS" -hostkey=*
put "$dstApp"       "${SFTP_PATH_FULL}/pwavplayer.bin"
put "$dstBoot"      "${SFTP_PATH_FULL}/bootloader.bin"
put "$dstPartition" "${SFTP_PATH_FULL}/partition-table.bin"
put "$dstOta"       "${SFTP_PATH_FULL}/ota_data_initial.bin"
exit
"@ | Set-Content $tmpScript -Encoding UTF8

        try {
            & $winscpExe /ini=nul /script=$tmpScript
            if ($LASTEXITCODE -ne 0) { throw "WinSCP exit $LASTEXITCODE" }
        } finally {
            Remove-Item $tmpScript -ErrorAction SilentlyContinue
        }

        Info "OK: Alle 4 Dateien erfolgreich hochgeladen."
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
Info "  pwavplayer.bin"
Info "  bootloader.bin"
Info "  partition-table.bin"
Info "  ota_data_initial.bin"
if ($SFTP_ENABLED) {
    Info "SFTP-Ziel: sftp://${SFTP_HOST}${SFTP_PATH_FULL}/"
} else {
    Warn "SFTP-Upload wurde uebersprungen (kein Passwort eingegeben)."
}
Write-Host "==================================================" -ForegroundColor Green
