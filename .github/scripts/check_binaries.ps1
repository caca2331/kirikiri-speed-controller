# Checks the built binaries for properties that only show up after linking:
# - the controller does not import SoundTouch.dll (dist no longer ships one beside it)
# - the hook delay-loads SoundTouch.dll instead of importing it
# - the controller embeds the comctl32 v6 + PerMonitorV2 manifest
param([Parameter(Mandatory = $true)][string]$BinDir)

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$dumpbin = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin.exe not found' }

function Get-Dependencies([string]$path) {
    $section = ''
    $normal = @()
    $delay = @()
    $output = & $dumpbin /nologo /dependents $path
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $path (exit $LASTEXITCODE)" }
    foreach ($line in $output) {
        if ($line -match 'following delay load dependencies') { $section = 'delay'; continue }
        if ($line -match 'following dependencies') { $section = 'normal'; continue }
        if ($line -match '^\s*Summary') { $section = ''; continue }
        $name = $line.Trim()
        if ($name -match '\.dll$') {
            if ($section -eq 'delay') { $delay += $name } elseif ($section -eq 'normal') { $normal += $name }
        }
    }
    # Every Windows image imports KERNEL32; an empty list means the output was not parsed.
    if ($normal.Count -eq 0) { throw "No imports parsed for $path; dumpbin output format changed?" }
    return @{ Normal = $normal; Delay = $delay }
}

$controller = Join-Path $BinDir 'KrkrSpeedController.exe'
$hook = Join-Path $BinDir 'krkr_speed_hook.dll'

$ctl = Get-Dependencies $controller
Write-Host "Controller imports: $($ctl.Normal -join ', ')"
if ($ctl.Normal -contains 'SoundTouch.dll' -or $ctl.Delay -contains 'SoundTouch.dll') {
    throw 'KrkrSpeedController.exe depends on SoundTouch.dll'
}

$hk = Get-Dependencies $hook
Write-Host "Hook imports: $($hk.Normal -join ', ') | delay-loaded: $($hk.Delay -join ', ')"
if ($hk.Normal -contains 'SoundTouch.dll') { throw 'krkr_speed_hook.dll imports SoundTouch.dll directly' }
if (-not ($hk.Delay -contains 'SoundTouch.dll')) { throw 'krkr_speed_hook.dll does not delay-load SoundTouch.dll' }

$mt = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\*\x64\mt.exe') |
    Sort-Object FullName | Select-Object -Last 1
if (-not $mt) { throw 'mt.exe not found' }
$manifestOut = Join-Path $env:RUNNER_TEMP 'controller.manifest'
& $mt.FullName -nologo "-inputresource:$controller;#1" "-out:$manifestOut"
if ($LASTEXITCODE -ne 0) { throw 'Could not extract the controller manifest' }
$manifest = Get-Content $manifestOut -Raw
if ($manifest -notmatch 'Microsoft\.Windows\.Common-Controls') { throw 'Controller manifest lacks comctl32 v6' }
if ($manifest -notmatch 'PerMonitorV2') { throw 'Controller manifest lacks PerMonitorV2 DPI awareness' }
Write-Host 'Binary checks passed'
