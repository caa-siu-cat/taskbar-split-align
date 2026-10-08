#Requires -Version 5.1
<#
.SYNOPSIS
    Installs or updates this mod in the local Windhawk installation.

.DESCRIPTION
    Copies the mod source into Windhawk's ModsSource directory, where Windhawk
    keeps the source of every local mod, and refreshes the compiled DLL.

    Windhawk has no command line interface, so the last step has to happen in its
    window: after this script finishes, toggle the mod off and on again in
    Windhawk (or restart Windhawk) so the new source is compiled and loaded.

    The mod id is read from the source file, and the file is written as
    local@<mod-id>.wh.cpp, which is how Windhawk names local mod source files.

.PARAMETER Source
    Mod source file. Defaults to taskbar-split-align.wh.cpp next to this script.

.PARAMETER SkipBuild
    Only copy the source; do not compile the DLL.

.EXAMPLE
    .\install-mod.ps1

.EXAMPLE
    .\install-mod.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Source,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-WindhawkDataPath {
    <#
        Resolves the directory that holds Windhawk's runtime data. Windhawk records
        it in windhawk.ini next to the executable as AppDataPath, which defaults to
        %ProgramData%\Windhawk.
    #>
    $candidates = @(
        (Join-Path $env:ProgramData 'Windhawk'),
        (Join-Path $env:LOCALAPPDATA 'Windhawk')
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'ModsSource'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Windhawk data directory not found. Looked for a ModsSource folder under: $($candidates -join ', ')"
}

function Get-ModMetadata {
    param([string]$Path, [string]$Field)

    $pattern = '^\s*//\s*@' + [regex]::Escape($Field) + '\s+(\S.*?)\s*$'
    $line = Get-Content -LiteralPath $Path -TotalCount 40 |
        Where-Object { $_ -match $pattern } |
        Select-Object -First 1
    if (-not $line) { return $null }
    return ([regex]::Match($line, $pattern)).Groups[1].Value.Trim()
}

# ---------------------------------------------------------------- resolve inputs

if (-not $Source) { $Source = Join-Path $PSScriptRoot 'taskbar-split-align.wh.cpp' }
if (-not (Test-Path -LiteralPath $Source)) { throw "Mod source '$Source' not found." }
$Source = (Resolve-Path -LiteralPath $Source).Path

$modId = Get-ModMetadata -Path $Source -Field 'id'
if (-not $modId) { throw "Could not read '@id' from '$Source'." }
$modVersion = Get-ModMetadata -Path $Source -Field 'version'
$modName = Get-ModMetadata -Path $Source -Field 'name'

$dataPath = Get-WindhawkDataPath
$modsSourceDirectory = Join-Path $dataPath 'ModsSource'
$target = Join-Path $modsSourceDirectory "local@$modId.wh.cpp"

# -------------------------------------------------------------- copy the source

Write-Host "Windhawk data : $dataPath"
Write-Host "Mod id        : $modId"
Write-Host "Name          : $modName"
Write-Host "Version       : $modVersion"
Write-Host "Source        : $Source"
Write-Host "Target        : $target"

$existed = Test-Path -LiteralPath $target
if ($existed) {
    $existingId = Get-ModMetadata -Path $target -Field 'id'
    Write-Host "Existing file : present (id '$existingId'), overwriting"
} else {
    Write-Host "Existing file : none, creating a new local mod"
}

Copy-Item -LiteralPath $Source -Destination $target -Force
Write-Host ''
Write-Host "Source copied to Windhawk."

# ------------------------------------------------------------ refresh the build

if (-not $SkipBuild) {
    $buildScript = Join-Path $PSScriptRoot 'build-mod.ps1'
    if (Test-Path -LiteralPath $buildScript) {
        Write-Host ''
        Write-Host 'Compiling...'
        & $buildScript -Source $Source | Out-Null
    } else {
        Write-Warning "build-mod.ps1 not found next to this script; skipped compilation."
    }
}

# ------------------------------------------------------------------- next steps

Write-Host ''
Write-Host 'Windhawk has no command line interface, so the mod is loaded by its UI.'
Write-Host 'Do this now:'
Write-Host "  1. Open Windhawk and go to the '$modName' page."
Write-Host '  2. Turn the mod off and on again (or restart Windhawk).'
Write-Host ''
Write-Host 'If the mod does not appear in the list at all, restart Windhawk: it reads'
Write-Host "the ModsSource folder, but only rescans it on startup."
