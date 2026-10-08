#Requires -Version 5.1
<#
.SYNOPSIS
    Compiles taskbar-split-align.wh.cpp into a Windhawk mod DLL.

.DESCRIPTION
    Uses the compiler and engine that ship with the locally installed Windhawk, so
    no extra toolchain is needed. The Windhawk directory, compiler and engine
    version are detected automatically and can be overridden with parameters or
    environment variables:

        WINDHAWK_PATH      Windhawk installation directory
        WINDHAWK_ENGINE    engine version directory name, for example 1.7.3

    Compiler arguments are passed in two ways on purpose: the two defines that
    embed string literals go through a clang response file, and everything else
    goes on the command line. A response file is tokenized by clang itself, so the
    quoting of the string literals behaves the same no matter which PowerShell
    hosts the build, while paths are left to the shell, which quotes them
    correctly. Passing the defines on the command line instead would break under
    Windows PowerShell 5.1 and PowerShell 7 in opposite ways.

.PARAMETER Version
    Mod version to embed. Defaults to the @version value in the mod source.

.PARAMETER Source
    Mod source file. Defaults to taskbar-split-align.wh.cpp next to this script.

.PARAMETER Output
    DLL output path. Defaults to build\local@<mod-id>_<version>.dll next to this script.

.EXAMPLE
    .\build-mod.ps1

.EXAMPLE
    .\build-mod.ps1 -Version 1.0.1
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Source,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-WindhawkPath {
    param([string]$Override)

    if ($Override) {
        if (-not (Test-Path -LiteralPath $Override)) {
            throw "Windhawk path '$Override' does not exist."
        }
        return (Resolve-Path -LiteralPath $Override).Path
    }

    $envOverride = [Environment]::GetEnvironmentVariable('WINDHAWK_PATH')
    if ($envOverride) {
        if (-not (Test-Path -LiteralPath $envOverride)) {
            throw "WINDHAWK_PATH '$envOverride' does not exist."
        }
        return (Resolve-Path -LiteralPath $envOverride).Path
    }

    # Best effort: the installer may record its location here. Windhawk 1.7.3 does
    # not, in which case the default installation directory below is used.
    $fromRegistry = $null
    try {
        $fromRegistry = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Windhawk' -ErrorAction Stop).InstallDir
    } catch {
        # Registry value absent: expected for portable installs and for 1.7.3.
    }
    if ($fromRegistry -and (Test-Path -LiteralPath $fromRegistry)) {
        return (Resolve-Path -LiteralPath $fromRegistry).Path
    }

    foreach ($candidate in @(
            (Join-Path ${env:ProgramFiles} 'Windhawk'),
            (Join-Path ${env:ProgramFiles(x86)} 'Windhawk')
        )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Windhawk was not found. Install it, or set WINDHAWK_PATH to its directory."
}

function Get-EngineVersion {
    param([string]$WindhawkPath, [string]$Override)

    if ($Override) {
        if (-not (Test-Path -LiteralPath (Join-Path $WindhawkPath "Engine\$Override"))) {
            throw "Engine version '$Override' is not present under '$WindhawkPath\Engine'."
        }
        return $Override
    }

    $envOverride = [Environment]::GetEnvironmentVariable('WINDHAWK_ENGINE')
    if ($envOverride) {
        return (Get-EngineVersion -WindhawkPath $WindhawkPath -Override $envOverride)
    }

    $engineRoot = Join-Path $WindhawkPath 'Engine'
    if (-not (Test-Path -LiteralPath $engineRoot)) {
        throw "No Engine directory found under '$WindhawkPath'."
    }

    # Pick the highest version directory, so a Windhawk update does not require
    # editing this script. Versions compare numerically (1.10 ranks above 1.9), and
    # directories such as 32 and 64 are skipped because they are not versions.
    $versions = @()
    foreach ($directory in (Get-ChildItem -LiteralPath $engineRoot -Directory)) {
        if ($directory.Name -notmatch '^(\d+(?:\.\d+)*)') { continue }
        try {
            $parsed = [version]$Matches[1]
        } catch {
            continue
        }
        $versions += [pscustomobject]@{ Name = $directory.Name; Parsed = $parsed }
    }

    if (-not $versions) {
        throw "No versioned engine directory found under '$engineRoot'."
    }
    return ($versions | Sort-Object -Property Parsed -Descending | Select-Object -First 1).Name
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

function Write-ResponseFile {
    <#
        Writes clang's @response file.

        Quotes inside a value have to be backslash-escaped here, because clang
        tokenizes the file itself and would otherwise strip them, turning
        L"local@<id>" into the invalid token Llocal@<id>.
    #>
    param([string]$Path, [string[]]$Arguments)

    $encoding = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($Path, $Arguments, $encoding)
}

function Assert-DefineQuoting {
    <#
        Proves that the response file mechanism delivers the mod id as a wide
        string literal before the real build relies on it. Compiles a throwaway
        program that only builds when the macros arrive intact.
    #>
    param([string]$CompilerExe, [string]$ModId, [string]$ModVersion)

    $probeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) 'windhawk-define-probe'
    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    $probeSource = Join-Path $probeDirectory 'probe.cpp'
    $probeArguments = Join-Path $probeDirectory 'probe.rsp'
    $probeBinary = Join-Path $probeDirectory 'probe.exe'

    Set-Content -LiteralPath $probeSource -Encoding ASCII -Value @(
        'int main() {'
        '    static_assert(sizeof(WH_MOD_ID) > 2, "mod id must be a wide string");'
        '    if (WH_MOD_ID[0] != L''l'') return 1;'
        '    if (WH_MOD_VERSION[0] < L''0'' || WH_MOD_VERSION[0] > L''9'') return 2;'
        '    return 0;'
        '}'
    )

    Write-ResponseFile -Path $probeArguments -Arguments @(
        "-DWH_MOD_ID=L\`"local@$ModId\`""
        "-DWH_MOD_VERSION=L\`"$ModVersion\`""
    )

    Remove-Item -LiteralPath $probeBinary -ErrorAction SilentlyContinue
    $diagnostics = & $CompilerExe -x c++ -std=c++23 "@$probeArguments" $probeSource -o $probeBinary 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $probeBinary)) {
        throw @"
Response file check failed: clang did not receive -DWH_MOD_ID as a wide string
literal, so the mod id would have been embedded as the invalid token
Llocal@$ModId. Compiler output:

$($diagnostics | Select-Object -First 10 | Out-String)
"@
    }

    Remove-Item -LiteralPath $probeDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- resolve inputs

$windhawkPath = Get-WindhawkPath
$engineVersion = Get-EngineVersion -WindhawkPath $windhawkPath

if (-not $Source) { $Source = Join-Path $PSScriptRoot 'taskbar-split-align.wh.cpp' }
if (-not (Test-Path -LiteralPath $Source)) { throw "Mod source '$Source' not found." }
$Source = (Resolve-Path -LiteralPath $Source).Path

$modId = Get-ModMetadata -Path $Source -Field 'id'
if (-not $modId) { throw "Could not read '@id' from '$Source'." }

if (-not $Version) {
    $Version = Get-ModMetadata -Path $Source -Field 'version'
    if (-not $Version) { throw "Could not read '@version' from '$Source'; pass -Version." }
}

$compilerDir = Join-Path $windhawkPath 'Compiler'
$compilerExe = Join-Path $compilerDir 'bin\clang++.exe'
$engineDir = Join-Path $windhawkPath "Engine\$engineVersion"
$apiHeader = Join-Path $compilerDir 'include\windhawk_api.h'
$engineLib = Join-Path $engineDir '64\windhawk.lib'

foreach ($required in @($compilerExe, $apiHeader, $engineLib)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required Windhawk file not found: $required"
    }
}

if (-not $Output) {
    $Output = Join-Path $PSScriptRoot "build\local@${modId}_${Version}.dll"
}
$outputDirectory = Split-Path -Parent $Output
if ($outputDirectory) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

# --------------------------------------------------------------------- compile

Write-Host "Windhawk      : $windhawkPath"
Write-Host "Engine        : $engineVersion"
Write-Host "Mod id        : $modId"
Write-Host "Version       : $Version"
Write-Host "Source        : $Source"
Write-Host "Output        : $Output"

Assert-DefineQuoting -CompilerExe $compilerExe -ModId $modId -ModVersion $Version

$responseFile = Join-Path ([System.IO.Path]::GetTempPath()) "windhawk-$modId.rsp"

# Only the defines go in the response file; see the note in the header. Quotes
# inside the value must be backslash-escaped for clang's own tokenizer.
Write-ResponseFile -Path $responseFile -Arguments @(
    "-DWH_MOD_ID=L\`"local@$modId\`""
    "-DWH_MOD_VERSION=L\`"$Version\`""
)

$arguments = @(
    '-std=c++23', '-O2', '-shared'
    '-DUNICODE', '-D_UNICODE'
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00'
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0'
    '-DWH_MOD'
    "@$responseFile"
    # The import library comes before -x c++, because -x applies to everything that
    # follows it and would make clang parse the library as C++ source.
    $engineLib
    '-include', 'windhawk_api.h'
    '-I', (Join-Path $compilerDir 'include')
    '-L', (Join-Path $engineDir '64')
    '-x', 'c++'
    $Source
    '-target', 'x86_64-w64-mingw32'
    '-Wl,--export-all-symbols'
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers'
    '-ldwmapi', '-lole32', '-loleaut32', '-lruntimeobject'
    '-o', $Output
)

# The compiler resolves its own headers and libraries relative to its working
# directory, so it has to run from the Compiler folder.
Push-Location -LiteralPath $compilerDir
try {
    & $compilerExe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

Remove-Item -LiteralPath $responseFile -Force -ErrorAction SilentlyContinue

$result = Get-Item -LiteralPath $Output
Write-Host ''
Write-Host "Built $($result.FullName) ($([math]::Round($result.Length / 1KB, 1)) KB)"
$result
