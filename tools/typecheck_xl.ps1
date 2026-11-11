# Typechecks the Windows-only XL layers on Windows -- the native counterpart of
# tools/typecheck_xl.sh.
#
# The .sh exists to compile xl/sdk and xl/interface on a machine that is NOT
# Windows, where they otherwise vanish behind the platform guard; it fakes
# <windows.h> with a ten-typedef shim. On Windows none of that is needed: this
# compiles the same two files against the REAL <windows.h> and the real Excel
# SDK header, so it catches strictly more -- including anything the shim's
# stubbed-out pascal/_cdecl/CALLBACK would hide.
#
# It is a fast -fsyntax-only check of just those two files, with no CMake
# configure. The full `cmake --preset release` build remains authoritative; this
# is the quick inner-loop gate to run after touching either file.
#
# Needs clang++ on PATH (ships with Visual Studio's "C++ Clang tools") and the
# SDK staged in xlsdk/ (see CLAUDE.md). Exits non-zero on any diagnostic.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root

if (-not (Test-Path 'xlsdk/xlcall.h')) {
    Write-Host 'typecheck_xl: no xlsdk/xlcall.h -- see CLAUDE.md for where the SDK goes'
    exit 2
}
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) {
    Write-Host 'typecheck_xl: clang++ not on PATH -- install the VS "C++ Clang tools" component'
    exit 2
}

$sources = @('xl/sdk/src/oper.cpp', 'xl/interface/src/exports.cpp')
foreach ($src in $sources) {
    Write-Host ('  {0,-34} ' -f $src) -NoNewline
    # Real <windows.h>; _WIN32 is predefined by clang, so the file's real branch
    # compiles without the CDSMODEL_XL_TYPECHECK shim escape hatch.
    $log = & clang++ -std=c++20 -fsyntax-only -fdeclspec `
        -I include -I xl/core/include -I xl/sdk/include -I xlsdk `
        $src 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host 'ok'
    } else {
        Write-Host 'FAILED'
        $log | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
        exit 1
    }
}
Write-Host 'both Windows-only layers typecheck against the real xlcall.h'
