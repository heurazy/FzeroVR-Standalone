param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root ".tools"
$sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"

$javaCandidates = @(
    (Join-Path $env:USERPROFILE ".jdks\jbr-17.0.14"),
    (Join-Path $env:USERPROFILE ".jdks\corretto-17.0.15")
)
$javaHome = $javaCandidates | Where-Object { Test-Path (Join-Path $_ "bin\java.exe") } | Select-Object -First 1
if (-not $javaHome) {
    throw "JDK 17 introuvable. Installe un JDK 17 ou adapte scripts/build-quest.ps1."
}
if (-not (Test-Path $sdk)) {
    throw "Android SDK introuvable: $sdk"
}

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk

$gradleVersion = "8.5"
$gradleHome = Join-Path $tools "gradle-$gradleVersion"
$gradleExe = Join-Path $gradleHome "bin\gradle.bat"
if (-not (Test-Path $gradleExe)) {
    New-Item -ItemType Directory -Force -Path $tools | Out-Null
    $zip = Join-Path $tools "gradle-$gradleVersion-bin.zip"
    if (-not (Test-Path $zip)) {
        Write-Host "Téléchargement de Gradle $gradleVersion..."
        Invoke-WebRequest "https://services.gradle.org/distributions/gradle-$gradleVersion-bin.zip" -OutFile $zip
    }
    Expand-Archive -Path $zip -DestinationPath $tools -Force
}

$task = if ($Configuration -eq "Release") { ":app:assembleRelease" } else { ":app:assembleDebug" }
Push-Location $root
try {
    & $gradleExe --no-daemon $task
    if ($LASTEXITCODE -ne 0) { throw "Gradle a échoué avec le code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$variant = $Configuration.ToLowerInvariant()
$apk = Join-Path $root "app\build\outputs\apk\$variant\app-$variant.apk"
if (Test-Path $apk) {
    Write-Host "APK: $apk" -ForegroundColor Green
} else {
    Write-Warning "Build terminé mais APK non trouvé à l'emplacement attendu."
}
