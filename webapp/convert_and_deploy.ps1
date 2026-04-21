#Requires -Version 5.1
# convert_and_deploy.ps1
#
# Wandelt README.md per Pandoc in README.html um und laedt sie zusammen mit
# PWAVplayer_config_editor.html per SFTP (WinSCP) auf den Server hoch.
#
# Voraussetzungen:
#   - Pandoc installiert und im PATH (https://pandoc.org)
#   - WinSCP installiert (wird automatisch gefunden)
#   - .env Datei im Projektverzeichnis oder im pwavplayer-Root (../)
#
# Verwendung:
#   .\convert_and_deploy.ps1

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
    Err ".env nicht gefunden (weder im aktuellen Verzeichnis noch im pwavplayer-Root ..\)."
    exit 1
}
Info ".env geladen aus: $envFile"

$envVars = @{}
Get-Content $envFile | Where-Object { $_ -match '^\s*[^#\s].+=.' } | ForEach-Object {
    $parts = $_ -split "=", 2
    $envVars[$parts[0].Trim()] = $parts[1].Trim()
}

foreach ($var in @("SFTP_HOST", "SFTP_USER", "SFTP_PATH")) {
    if (-not $envVars.ContainsKey($var) -or [string]::IsNullOrEmpty($envVars[$var])) {
        Err "Variable '$var' fehlt oder ist leer in $envFile"
        exit 1
    }
}

$SFTP_HOST = $envVars["SFTP_HOST"]
$SFTP_USER = $envVars["SFTP_USER"]
$SFTP_PATH = $envVars["SFTP_PATH"].TrimEnd('/')

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

# -- Markdown -> HTML konvertieren --------------------------------------------
$conversions = @(
    @{ Src = "README.md"; Dst = "README.html" }
)

foreach ($c in $conversions) {
    if (-not (Test-Path $c.Src)) {
        Err "Quelldatei nicht gefunden: $($c.Src)"
        exit 1
    }
    Info "Konvertiere $($c.Src) -> $($c.Dst) ..."
    pandoc -s -f markdown+gfm_auto_identifiers -o $c.Dst $c.Src
    if ($LASTEXITCODE -ne 0) {
        Err "Pandoc fehlgeschlagen fuer $($c.Src) (exit $LASTEXITCODE)"
        exit 1
    }
    Info "OK: $($c.Dst) erstellt."
}

# -- SFTP-Upload per WinSCP ---------------------------------------------------
if ($SFTP_ENABLED) {
    StepMsg "SFTP-Upload"

    $uploadFiles = @(
        "README.html",
        "PWAVplayer_config_editor.html"
    )

    $putCommands = ($uploadFiles | ForEach-Object {
        $localPath = (Resolve-Path $_).Path
        "put `"$localPath`" `"${SFTP_PATH}/$_`""
    }) -join "`n"

    $tmpScript = [System.IO.Path]::GetTempFileName()
    @"
open sftp://${SFTP_USER}@${SFTP_HOST}/ -password="$SFTP_PASS" -hostkey=*
$putCommands
exit
"@ | Set-Content $tmpScript -Encoding UTF8

    try {
        Info "Lade $($uploadFiles.Count) Dateien hoch nach sftp://${SFTP_HOST}${SFTP_PATH}/ ..."
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
Info "Konvertierung abgeschlossen!"
Info "Erstellte HTML-Dateien:"
foreach ($c in $conversions) {
    Info "  $($c.Dst)"
}
if ($SFTP_ENABLED) {
    Info "SFTP-Ziel: sftp://${SFTP_HOST}${SFTP_PATH}/"
} else {
    Warn "SFTP-Upload wurde uebersprungen (kein Passwort eingegeben)."
}
Write-Host "==================================================" -ForegroundColor Green
