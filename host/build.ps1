param([string]$Game = 'gangstar', [switch]$Release, [switch]$Dev, [switch]$NoIcon)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:PATH = "C:\msys64\mingw32\bin;$env:PATH"
foreach ($tool in 'gcc', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "no $tool`: in MSYS2, pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja"
    }
}
$kind = if ($Release) { 'release' } elseif ($Dev) { 'dev' } else { 'plain' }
$icon = if ($NoIcon) { 'ON' } else { 'OFF' }
$bin = Join-Path $here "..\build\cmake\$Game-$kind"
& cmake -S $here -B $bin -G Ninja -DCMAKE_C_COMPILER=gcc "-DGMV_GAME=$Game" "-DGMV_BUILD=$kind" "-DGMV_NOICON=$icon" `
    '-DGMV_OUT_DIR=' '-DGMV_STAGE1=OFF'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build $bin
exit $LASTEXITCODE
