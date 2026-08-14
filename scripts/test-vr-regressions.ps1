$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$generatedCourse = Join-Path $root 'app\.cxx\Debug\3o2s363q\arm64-v8a\gdx_quest_generated\course_quest.c'
$settingsJava = Join-Path $root 'app\src\main\java\com\fzerox\vr\VrSettingsActivity.java'
$openXrCpp = Join-Path $root 'app\src\main\cpp\openxr_context.cpp'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

Write-Host 'TEST diorama full-ring group capacity...'
Assert-True (Test-Path $generatedCourse) "Generated course source not found: $generatedCourse"
$course = Get-Content $generatedCourse -Raw
$groupMatch = [regex]::Match($course, '#define\s+SEGMENT_CHUNK_GROUP_COUNT\s+(0x[0-9A-Fa-f]+|\d+)')
$chunkMatch = [regex]::Match($course, '#define\s+SEGMENT_CHUNK_COUNT\s+(0x[0-9A-Fa-f]+|\d+)')
Assert-True $groupMatch.Success 'SEGMENT_CHUNK_GROUP_COUNT not found'
Assert-True $chunkMatch.Success 'SEGMENT_CHUNK_COUNT not found'
function Parse-CInt([string]$value) {
    if ($value.StartsWith('0x', [System.StringComparison]::OrdinalIgnoreCase)) {
        return [Convert]::ToInt32($value.Substring(2), 16)
    }
    return [int]$value
}
$groupCount = Parse-CInt $groupMatch.Groups[1].Value
$chunkCount = Parse-CInt $chunkMatch.Groups[1].Value
Assert-True ($groupCount -gt $chunkCount) "Diorama can split a full ring into up to $chunkCount groups, but only $groupCount group slots exist; the post-group increment reaches the end sentinel and returns before drawing the track."
Write-Host "PASS group capacity: $groupCount > $chunkCount"

Write-Host 'TEST 90 Hz defaults...'
$settings = Get-Content $settingsJava -Raw
$openxr = Get-Content $openXrCpp -Raw
Assert-True ($settings -match 'prefs\.getInt\(KEY_REFRESH,\s*90\)') 'VR settings UI default is not 90 Hz'
Assert-True ($settings -match 'if\s*\(hz\s*==\s*72\)\s*refreshGroup\.check\(R\.id\.refresh_72\)') 'Saved 72 Hz selection is not preserved in the UI'
Assert-True ($settings -match 'if\s*\(id\s*==\s*R\.id\.refresh_72\)\s*return\s+72;') '72 Hz radio option no longer returns 72 Hz'
Assert-True (($openxr | Select-String -Pattern 'return 90\.0f;' -AllMatches).Matches.Count -ge 2) 'OpenXR fallback/default refresh is not 90 Hz'
Assert-True ($openxr -match 'float\s+target\s*=\s*90\.0f;') 'OpenXR target refresh initialization is not 90 Hz'
Write-Host 'PASS 90 Hz defaults'
