param(
    [string]$Revision = "719fd82a3af605b064fb53ad6eecb020090b4c5d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
$dst = Join-Path $thirdParty "G-Diffuser"

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

if (-not (Test-Path (Join-Path $dst ".git"))) {
    git clone --recursive https://github.com/Zorkats/G-Diffuser.git $dst
}

Push-Location $dst
try {
    git fetch origin
    git checkout $Revision
    git submodule sync --recursive
    git submodule update --init --recursive
    Write-Host "G-Diffuser vendored at $Revision" -ForegroundColor Green
} finally {
    Pop-Location
}
