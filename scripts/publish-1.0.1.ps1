$ErrorActionPreference = 'Stop'

$repoUrl = 'https://github.com/heurazy/FzeroVR-Standalone.git'
$repoOwner = 'heurazy'
$repoName = 'FzeroVR-Standalone'
$tag = 'v1.0.1'
$releaseName = 'F-Zero X VR Standalone v1.0.1'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$apk = Join-Path $root 'app\build\outputs\apk\debug\app-debug.apk'
$notesPath = Join-Path $root 'RELEASE_NOTES_1.0.1.md'

Set-Location $root

if (-not (Test-Path $apk)) { throw "APK not found: $apk" }
if (-not (Test-Path $notesPath)) { throw "Release notes not found: $notesPath" }

$metadata = Join-Path $root 'app\build\outputs\apk\debug\output-metadata.json'
if (-not (Test-Path $metadata)) { throw "APK metadata not found: $metadata" }
$apkMeta = Get-Content $metadata -Raw | ConvertFrom-Json
if ($apkMeta.elements[0].versionName -ne '1.0.1') {
    throw "Expected APK version 1.0.1, got $($apkMeta.elements[0].versionName)"
}
if ([int]$apkMeta.elements[0].versionCode -ne 2) {
    throw "Expected APK versionCode 2, got $($apkMeta.elements[0].versionCode)"
}

Write-Host 'Publishing F-Zero X VR Standalone v1.0.1...' -ForegroundColor Cyan

$remotes = @(git remote)
if ($LASTEXITCODE -ne 0) { throw 'git remote failed' }
if ($remotes -notcontains 'origin') {
    git remote add origin $repoUrl
    if ($LASTEXITCODE -ne 0) { throw 'git remote add origin failed' }
} else {
    $origin = (git remote get-url origin).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'git remote get-url origin failed' }
    if ($origin -ne $repoUrl) {
        git remote set-url origin $repoUrl
        if ($LASTEXITCODE -ne 0) { throw 'git remote set-url origin failed' }
    }
}

git fetch origin main
if ($LASTEXITCODE -ne 0) { throw 'git fetch origin main failed' }

$branch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0) { throw 'git branch failed' }
if ($branch -ne 'main') { throw "Expected main branch, got '$branch'" }

$behind = (git rev-list --count HEAD..origin/main).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Unable to compare origin/main' }
if ([int]$behind -gt 0) {
    throw "origin/main has $behind commit(s) not present locally. Pull/reconcile before publishing; refusing to force-push."
}

git add -A
if ($LASTEXITCODE -ne 0) { throw 'git add failed' }

$hasChanges = -not [string]::IsNullOrWhiteSpace((git status --porcelain))
if ($hasChanges) {
    git commit -m 'Release F-Zero X VR Standalone v1.0.1'
    if ($LASTEXITCODE -ne 0) { throw 'git commit failed' }
} else {
    Write-Host 'No source changes to commit.' -ForegroundColor Yellow
}

git push -u origin main
if ($LASTEXITCODE -ne 0) { throw 'git push main failed' }

$remoteTag = git ls-remote --tags origin "refs/tags/$tag"
if ([string]::IsNullOrWhiteSpace($remoteTag)) {
    git tag -a $tag -m $releaseName
    if ($LASTEXITCODE -ne 0) { throw 'git tag failed' }
    git push origin $tag
    if ($LASTEXITCODE -ne 0) { throw 'git push tag failed' }
} else {
    Write-Host "$tag already exists on origin." -ForegroundColor Yellow
}

# Obtain the existing GitHub credential through Git Credential Manager without printing it.
$credentialRequest = "protocol=https`nhost=github.com`n`n"
$credentialLines = $credentialRequest | git credential fill
if ($LASTEXITCODE -ne 0) { throw 'Unable to obtain GitHub credentials from Git Credential Manager' }

$credential = @{}
foreach ($line in $credentialLines) {
    $parts = $line -split '=', 2
    if ($parts.Count -eq 2) { $credential[$parts[0]] = $parts[1] }
}
if (-not $credential.ContainsKey('password')) {
    throw 'Git Credential Manager did not return a GitHub token/password'
}

$headers = @{
    Authorization = "Bearer $($credential['password'])"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
    'User-Agent' = 'FzeroVR-Standalone-Release-Script'
}

$apiBase = "https://api.github.com/repos/$repoOwner/$repoName"
$release = $null
try {
    $release = Invoke-RestMethod -Method Get -Uri "$apiBase/releases/tags/$tag" -Headers $headers
    Write-Host 'GitHub release already exists; reusing it.' -ForegroundColor Yellow
} catch {
    $statusCode = $null
    if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
        $statusCode = [int]$_.Exception.Response.StatusCode
    }
    if ($statusCode -ne 404) { throw }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $releaseBody = [string][System.IO.File]::ReadAllText($notesPath, $utf8)
    $payloadObject = @{
        tag_name = [string]$tag
        target_commitish = 'main'
        name = [string]$releaseName
        body = [string]$releaseBody
        draft = $false
        prerelease = $false
        generate_release_notes = $false
    }
    $payload = $payloadObject | ConvertTo-Json -Depth 4 -Compress
    $payloadBytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
    $release = Invoke-RestMethod -Method Post -Uri "$apiBase/releases" -Headers $headers -ContentType 'application/json; charset=utf-8' -Body $payloadBytes
}

$assetName = 'FzeroVR-Standalone-v1.0.1-Quest3.apk'
$existingAsset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1
if ($existingAsset) {
    Write-Host "Release asset $assetName already exists; leaving it unchanged." -ForegroundColor Yellow
} else {
    $uploadUrl = ($release.upload_url -replace '\{\?name,label\}$', '') + '?name=' + [uri]::EscapeDataString($assetName)
    Invoke-RestMethod -Method Post -Uri $uploadUrl -Headers $headers -ContentType 'application/vnd.android.package-archive' -InFile $apk | Out-Null
    Write-Host "Uploaded $assetName" -ForegroundColor Green
}

Write-Host ''
Write-Host 'Published successfully:' -ForegroundColor Green
Write-Host "https://github.com/$repoOwner/$repoName/releases/tag/$tag"
