param(
    [Parameter(Mandatory=$true)]
    [string]$Rom,
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
$adb = Join-Path $sdk "platform-tools\adb.exe"
$apk = Join-Path $root "app\build\outputs\apk\debug\app-debug.apk"

if (-not (Test-Path $adb)) { throw "adb introuvable: $adb" }
if (-not (Test-Path $apk)) { throw "APK introuvable. Lance d'abord scripts/build-quest.ps1." }
if (-not (Test-Path $Rom)) { throw "ROM introuvable: $Rom" }

$romInfo = Get-Item $Rom
if ($romInfo.Length -lt 8MB -or $romInfo.Length -gt 64MB) {
    throw "Taille ROM inattendue ($($romInfo.Length) octets). Utilise un dump F-Zero X US rev0 .z64/.n64/.v64 valide."
}

$devices = & $adb devices
if (($devices | Select-String "\tdevice$").Count -lt 1) {
    throw "Aucun Quest connecté en ADB. Active le mode développeur puis accepte l'autorisation USB dans le casque."
}

Write-Host "Installation de l'APK Quest..." -ForegroundColor Cyan
& $adb install -r $apk
if ($LASTEXITCODE -ne 0) { throw "adb install a échoué ($LASTEXITCODE)" }

# Debug APK: run-as gives access to the app-private files directory without root.
# /data/local/tmp avoids Android scoped-storage restrictions while remaining readable by run-as.
$remoteTmp = "/data/local/tmp/fzeroxvr-baserom.tmp"
Write-Host "Copie de la ROM utilisateur dans le stockage privé de l'app..." -ForegroundColor Cyan
& $adb push $Rom $remoteTmp
if ($LASTEXITCODE -ne 0) { throw "adb push a échoué ($LASTEXITCODE)" }
& $adb shell run-as com.fzerox.vr mkdir -p files
if ($LASTEXITCODE -ne 0) { throw "run-as n'est pas disponible pour cette build debug." }
& $adb shell run-as com.fzerox.vr cp $remoteTmp files/baserom.us.rev0.z64
if ($LASTEXITCODE -ne 0) { throw "Impossible de copier la ROM dans files/." }
& $adb shell rm -f $remoteTmp | Out-Null

Write-Host "ROM installée sous files/baserom.us.rev0.z64" -ForegroundColor Green

if (-not $NoLaunch) {
    & $adb shell am force-stop com.fzerox.vr | Out-Null
    & $adb shell am start -n com.fzerox.vr/android.app.NativeActivity
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "APK installé, mais le lancement ADB a échoué. Lance F-Zero X VR depuis Applications > Sources inconnues."
    }
}

Write-Host "Pour les logs: adb logcat -s FZeroXVR FZeroXVR/OpenXR FZeroXVR/GameHost FZeroXVR/GDiffuser" -ForegroundColor DarkGray
