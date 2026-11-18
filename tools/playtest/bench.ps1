# Runs the bench-city scenario (or another) on one exe and one GPU, and sums up its [perf] lines.
#   tools\playtest\bench.ps1 -Exe build\dev\GangstarMV.exe -Gpu amd|nvidia|default [-Runs 2] [-Scenario bench-city]
#     [-Ini KEY=VALUE,...]   extra ini lines for the run (A/B of a switch on one exe)
#   tools\playtest\bench.ps1 -Log <GangstarMV.log>    only the summary of a log already made
# -Gpu sets Windows' graphics preference (HKCU\Software\Microsoft\DirectX\UserGpuPreferences) for the run's exe only:
# amd = power saving, nvidia = high performance. The value is always taken out again (or put back as it was) in a
# finally block, so an interrupted run leaves nothing behind. The game window takes the mouse and keyboard while it runs.
# The summary takes the last 10 [perf] windows: 6 standing still, then 4 walking (bench-city's order).
param(
    [string]$Exe,
    [ValidateSet('amd', 'nvidia', 'default')][string]$Gpu = 'default',
    [int]$Runs = 1,
    [string]$Scenario = 'bench-city',
    [string[]]$Ini = @(),
    [string]$Log
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$runExe = Join-Path $root 'build\playtest\run\GangstarMV.exe'
$key = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
$scenFile = Join-Path $here "scenarios\$Scenario.txt"

function Show-Perf([string]$log) {
    $t = New-Object System.Collections.ArrayList; $g = New-Object System.Collections.ArrayList
    $s = New-Object System.Collections.ArrayList
    foreach ($l in Get-Content $log) {
        if ($l -match '^\[perf\] ([\d.]+) s: (\d+) ticks .*?, (\d+) drew a frame \(([\d.]+) fps\); work a tick: drawn ([\d.]+) ms \(max ([\d.]+)\).*?file (\d+)') {
            [void]$t.Add(@{ fps = [double]$Matches[4]; drawn = [double]$Matches[5]; max = [double]$Matches[6]; file = [double]$Matches[7] })
        } elseif ($l -match '^\[perf\] GPU, (\d+) frames \((\d+) with the post pass\): scene ([\d.]+) ms, then sea reflections ([\d.]+), car reflections ([\d.]+), AO ([\d.]+), bloom ([\d.]+) .*?2D/HUD ([\d.]+), present ([\d.]+); first draw to present ([\d.]+) ms .*?; (\d+) draws a frame .*?CPU in the post pass ([\d.]+) ms') {
            [void]$g.Add(@{ scene = [double]$Matches[3]; sea = [double]$Matches[4]; car = [double]$Matches[5]; ao = [double]$Matches[6]; bloom = [double]$Matches[7]; hud = [double]$Matches[8]; pres = [double]$Matches[9]; gpu = [double]$Matches[10]; draws = [double]$Matches[11]; cpupost = [double]$Matches[12] })
        } elseif ($l -match '^\[perf\]   \d+ samples in ticks .*?: (.*)$') {
            $h = @{}; foreach ($m in [regex]::Matches($Matches[1], '(\S+) (\d+)%')) { $h[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
            [void]$s.Add($h)
        }
    }
    $avg = { param($a, $f) $v = 0.0; foreach ($x in $a) { $v += [double]$x[$f] }; if ($a.Count) { $v / $a.Count } else { 0 } }
    $mx = { param($a, $f) $m = 0.0; foreach ($x in $a) { if ([double]$x[$f] -gt $m) { $m = [double]$x[$f] } }; $m }
    $last = { param($a, $k) @($a | Select-Object -Last 10) }
    $tc = & $last $t; $gc = & $last $g; $sc = & $last $s
    $still = @($tc | Select-Object -First 6); $walk = @($tc | Select-Object -Last 4); $gs = @($gc | Select-Object -First 6)
    $ss = @($sc | Select-Object -First 6)
    $rend = (Select-String -Path $log -Pattern '^\[perf\] GPU timer queries ready \((.*)\)' | Select-Object -First 1).Matches.Groups[1].Value
    Write-Host "$rend  ($log)"
    '  still  fps {0,6:N1}  CPU drawn {1,5:N2} ms (max {2,5:N1})  file {3,4:N0} ms/s   ({4} windows of {5})' -f (& $avg $still 'fps'), (& $avg $still 'drawn'), (& $mx $still 'max'), (& $avg $still 'file'), $still.Count, $t.Count | Write-Host
    '  walk   fps {0,6:N1}  CPU drawn {1,5:N2} ms (max {2,5:N1})  file {3,4:N0} ms/s   ({4} windows)' -f (& $avg $walk 'fps'), (& $avg $walk 'drawn'), (& $mx $walk 'max'), (& $avg $walk 'file'), $walk.Count | Write-Host
    '  GPU still: scene {0:N2}, shadows+sea {1:N2}, cars {2:N2}, AO {3:N2}, bloom {4:N2}, HUD {5:N2}, present {6:N2}: {7:N2} ms a frame; {8:N0} draws; CPU in the post pass {9:N2} ms' -f (& $avg $gs 'scene'), (& $avg $gs 'sea'), (& $avg $gs 'car'), (& $avg $gs 'ao'), (& $avg $gs 'bloom'), (& $avg $gs 'hud'), (& $avg $gs 'pres'), (& $avg $gs 'gpu'), (& $avg $gs 'draws'), (& $avg $gs 'cpupost') | Write-Host
    $mods = @{}; foreach ($h in $ss) { foreach ($k in $h.Keys) { $mods[$k] = 1 } }
    $share = ($mods.Keys | ForEach-Object { [pscustomobject]@{ n = $_; v = (& $avg $ss $_) } } | Sort-Object v -Descending | Select-Object -First 5 | ForEach-Object { '{0} {1:N0}%' -f $_.n, $_.v }) -join ', '
    Write-Host "  CPU samples still: $share"
}

if ($Log) { Show-Perf (Resolve-Path $Log).Path; return }
if (-not $Exe) { throw '-Exe or -Log' }
$Exe = (Resolve-Path $Exe).Path
$scenBackup = $null; $prev = $null; $had = $false
try {
    if ($Ini.Count) {                                   # extra ini lines: the scenario file gets them for this run only
        $scenBackup = Get-Content $scenFile -Raw
        $text = $scenBackup -replace '(?m)^save ', ((($Ini | ForEach-Object { "ini $_" }) -join "`r`n") + "`r`nsave ")
        Set-Content $scenFile $text -NoNewline
    }
    if ($Gpu -ne 'default') {
        if (-not (Test-Path $key)) { New-Item $key -Force | Out-Null }
        $p = Get-ItemProperty $key -Name $runExe -ErrorAction SilentlyContinue
        if ($p) { $had = $true; $prev = $p.$runExe }
        Set-ItemProperty $key -Name $runExe -Value ($(if ($Gpu -eq 'nvidia') { 'GpuPreference=2;' } else { 'GpuPreference=1;' }))
    }
    for ($r = 1; $r -le $Runs; $r++) {
        & (Join-Path $here 'playtest.ps1') $Scenario -Exe $Exe | Select-Object -Last 4 | Out-Host
        $res = Get-ChildItem (Join-Path $root 'build\playtest\results') -Directory | Sort-Object Name | Select-Object -Last 1
        $lg = Get-ChildItem (Join-Path $res.FullName $Scenario) -Filter *.log | Select-Object -First 1
        if (-not $lg) { Write-Host "run $r`: no log"; continue }
        Write-Host "run $r of $Runs$(if ($Ini.Count) { ' (' + ($Ini -join ' ') + ')' })"
        Show-Perf $lg.FullName
    }
} finally {
    if ($scenBackup) { Set-Content $scenFile $scenBackup -NoNewline }
    if ($Gpu -ne 'default') {
        if ($had) { Set-ItemProperty $key -Name $runExe -Value $prev }
        else { Remove-ItemProperty $key -Name $runExe -ErrorAction SilentlyContinue }
        $left = Get-ItemProperty $key -Name $runExe -ErrorAction SilentlyContinue
        Write-Host ("GPU preference for the run exe: " + $(if ($had) { "put back ($prev)" } elseif ($left) { 'STILL THERE' } else { 'removed' }))
    }
}
