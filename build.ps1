param([string]$Version = '1.0.0')
$ErrorActionPreference = 'Stop'
$compilerDir = 'C:\Program Files\Windhawk\Compiler'
$source = Join-Path $PSScriptRoot 'taskbar-split-align.wh.cpp'
$output = Join-Path $PSScriptRoot "build\local@taskbar-split-align_${Version}.dll"
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'build') -Force | Out-Null
$arguments = @(
    '-std=c++23', '-O2', '-shared', '-DUNICODE', '-D_UNICODE',
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-DWH_MOD_ID=L"local@taskbar-split-align"', "-DWH_MOD_VERSION=L`"$Version`"",
    'C:\Program Files\Windhawk\Engine\1.7.3\64\windhawk.lib',
    '-x', 'c++', $source, '-include', 'windhawk_api.h',
    '-target', 'x86_64-w64-mingw32', '-Wl,--export-all-symbols',
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
    '-ldwmapi', '-lole32', '-loleaut32', '-lruntimeobject', '-o', $output
)
Push-Location $compilerDir
try {
    & '.\bin\clang++.exe' @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
} finally { Pop-Location }
Get-Item -LiteralPath $output | Select-Object FullName,Length
