param(
    [Parameter(Position = 0)][string[]]$Scenario = @('*'),
    [string]$Exe,
    [switch]$List,
    [int]$HangSeconds = 15
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$build = Join-Path $root 'build'
if (-not $Exe) { $Exe = Join-Path $build 'GangstarMV.exe' }
$Exe = (Resolve-Path $Exe).Path
$app = Join-Path $root 'GangstarMV.app'
$scenDir = Join-Path $here 'scenarios'

$all = @(Get-ChildItem $scenDir -Filter '*.txt' | Sort-Object Name)
$named = -not ($Scenario.Count -eq 1 -and $Scenario[0] -eq '*')
$chosen = @($all | Where-Object { $n = $_.BaseName; ($named -or $n -notlike 'selftest-*') -and @($Scenario | Where-Object { $n -like $_ }).Count })
if ($List) { $all | ForEach-Object { '{0,-24} {1}' -f $_.BaseName, ((Get-Content $_.FullName -TotalCount 1) -replace '^#\s*', '') }; return }
if (-not $chosen.Count) { throw "no scenario matches: $Scenario" }
if (Get-Process GangstarMV -ErrorAction SilentlyContinue) { throw 'GangstarMV is already running: close it first' }

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Text; using System.Drawing; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class PT {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsHungAppWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] static extern uint MapVirtualKeyW(uint c, uint t);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadKeyboardLayoutW(string id, uint fl);
  [DllImport("user32.dll")] static extern bool UnloadKeyboardLayout(IntPtr l);
  [DllImport("user32.dll")] static extern IntPtr GetKeyboardLayout(uint tid);
  [DllImport("user32.dll")] static extern int GetKeyboardLayoutList(int n, IntPtr[] l);
  [DllImport("user32.dll")] static extern IntPtr ActivateKeyboardLayout(IntPtr l, uint fl);
  static IntPtr oldLayout, newLayout; static bool loadedNew;
  // WM_INPUTLANGCHANGEREQUEST; the layout is taken out of the session again if it was not there before.
  public static bool SetLayout(IntPtr h, string id) {
    uint pid; oldLayout = GetKeyboardLayout(GetWindowThreadProcessId(h, out pid));
    IntPtr[] had = new IntPtr[64]; int n = GetKeyboardLayoutList(64, had);
    newLayout = LoadKeyboardLayoutW(id, 0x80);       // KLF_NOTELLSHELL
    if (newLayout == IntPtr.Zero) return false;
    loadedNew = Array.IndexOf(had, newLayout, 0, n) < 0;
    return PostMessageW(h, 0x50, IntPtr.Zero, newLayout);
  }
  public static void RestoreLayout(IntPtr h, bool alive) {
    if (newLayout == IntPtr.Zero) return;
    ActivateKeyboardLayout(oldLayout, 0);
    if (alive) { PostMessageW(h, 0x50, IntPtr.Zero, oldLayout); System.Threading.Thread.Sleep(200); }
    if (loadedNew) UnloadKeyboardLayout(newLayout);
    newLayout = IntPtr.Zero;
  }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
  // The process's visible window with a title that is no dialog (SDL keeps hidden helper windows too).
  public static IntPtr GameWindow(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      StringBuilder c = new StringBuilder(64), t = new StringBuilder(64);
      GetClassNameW(h, c, 64); GetWindowTextW(h, t, 64);
      if (c.ToString() == "#32770" || t.Length == 0) return true;
      found = h;
      return false;
    }, IntPtr.Zero);
    return found;
  }
  public static bool Hung(IntPtr h) { return IsHungAppWindow(h); }
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  // Per-monitor aware, so that with display scaling above 100 % the client rectangle, the clicks and the screen copy all
  // count physical pixels (unaware, the copy took the window's scaled size at an unscaled place: a cut-off picture).
  public static void DpiAware() {
    if (!SetProcessDpiAwarenessContext((IntPtr)(-4))) SetThreadDpiAwarenessContext((IntPtr)(-4));
  }
  public static bool HasFocus(IntPtr h) { return GetForegroundWindow() == h; }
  // Windows only lets the process that has the foreground give it away; a tap of Alt counts as input of ours.
  public static bool TakeFocus(IntPtr h) {
    if (GetForegroundWindow() == h) return true;
    keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
    BringWindowToTop(h); SetForegroundWindow(h);
    return GetForegroundWindow() == h;
  }
  public static void Client(IntPtr h, out int x, out int y, out int w, out int hh) {
    RECT r; GetClientRect(h, out r); POINT p = new POINT(); ClientToScreen(h, ref p);
    x = p.X; y = p.Y; w = r.R; hh = r.B;
  }
  public static void Point(IntPtr h, double fx, double fy) {
    int x, y, w, hh; Client(h, out x, out y, out w, out hh);
    SetCursorPos(x + (int)(fx * w), y + (int)(fy * hh));
  }
  // WM_SYSCOMMAND SC_SIZE + WMSZ_BOTTOMRIGHT: the corner moves as far as the mouse does from where it is, until a click ends it.
  public static void SysCommand(IntPtr h, int cmd) { PostMessageW(h, 0x112, (IntPtr)cmd, IntPtr.Zero); }
  public static void Drag(IntPtr h, int dx, int dy, string midShot) {
    PostMessageW(h, 0x112, (IntPtr)0xF008, IntPtr.Zero); System.Threading.Thread.Sleep(300);
    POINT p; GetCursorPos(out p);
    int x = p.X, y = p.Y;
    for (int i = 1; i <= 10; i++) { SetCursorPos(x + dx * i / 10, y + dy * i / 10); System.Threading.Thread.Sleep(40); }
    System.Threading.Thread.Sleep(150);
    if (!string.IsNullOrEmpty(midShot)) Shot(h, midShot, 960);
    Button(2); System.Threading.Thread.Sleep(100);
    Button(4); System.Threading.Thread.Sleep(300);
  }
  static void Send(INPUT i) { SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT))); }
  public static void Button(uint flag) { INPUT i = new INPUT(); i.type = 0; i.u.mi.flags = flag; Send(i); }
  public static void Rel(int dx, int dy) { INPUT i = new INPUT(); i.type = 0; i.u.mi.dx = dx; i.u.mi.dy = dy; i.u.mi.flags = 1; Send(i); }
  public static void Key(int vk, bool down) {
    INPUT i = new INPUT(); i.type = 1; i.u.ki.vk = (ushort)vk; i.u.ki.scan = (ushort)MapVirtualKeyW((uint)vk, 0);
    i.u.ki.flags = down ? 0u : 2u;
    if (vk >= 0x25 && vk <= 0x28) i.u.ki.flags |= 1;          // the arrows are extended keys
    Send(i);
  }
  // Windows names an injected scancode by the sender's layout, not the window's, unlike a real keyboard.
  public static void Scan(int scan, bool down) {
    if (newLayout != IntPtr.Zero) ActivateKeyboardLayout(newLayout, 0);
    INPUT i = new INPUT(); i.type = 1; i.u.ki.scan = (ushort)scan; i.u.ki.flags = down ? 8u : 10u;   // KEYEVENTF_SCANCODE
    Send(i);
  }
  // How far two shots are apart, 0-255 per pixel, on small copies so that noise counts for little.
  public static double Apart(string a, string b) {
    using (Bitmap fa = new Bitmap(a)) using (Bitmap fb = new Bitmap(b))
    using (Bitmap x = new Bitmap(fa, 96, 54)) using (Bitmap y = new Bitmap(fb, 96, 54)) {
      double sum = 0;
      for (int j = 0; j < 54; j++) for (int i = 0; i < 96; i++) {
        Color p = x.GetPixel(i, j), q = y.GetPixel(i, j);
        sum += (Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B)) / 3.0;
      }
      return sum / (96 * 54);
    }
  }
  // How much brighter the picture's top row and left column are than the row and column two pixels in (0 to 255):
  // a line along the edge that a full-screen draw left uncovered.  Needs a shot at the window's own size.
  public static double EdgeLeak(string a) {
    using (Bitmap b = new Bitmap(a)) {
      int w = b.Width, h = b.Height;
      double r0 = 0, r2 = 0, c0 = 0, c2 = 0;
      for (int x = 0; x < w; x++) { r0 += Lum(b.GetPixel(x, 0)); r2 += Lum(b.GetPixel(x, 2)); }
      for (int y = 0; y < h; y++) { c0 += Lum(b.GetPixel(0, y)); c2 += Lum(b.GetPixel(2, y)); }
      return Math.Max((r0 - r2) / w, (c0 - c2) / h);
    }
  }
  static double Lum(Color c) { return (c.R + c.G + c.B) / 3.0; }
  // The same, for the part of the picture where the main menu has its buttons, CONTINUE to QUIT.
  public static double ApartButtons(string a, string b) {
    using (Bitmap fa = new Bitmap(a)) using (Bitmap fb = new Bitmap(b))
    using (Bitmap x = new Bitmap(fa, 96, 54)) using (Bitmap y = new Bitmap(fb, 96, 54)) {
      double sum = 0; int n = 0;
      for (int j = 19; j < 49; j++) for (int i = 1; i < 20; i++) {
        Color p = x.GetPixel(i, j), q = y.GetPixel(i, j);
        sum += (Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B)) / 3.0; n++;
      }
      return sum / n;
    }
  }
  public static void Shot(IntPtr h, string path, int width) {
    int x, y, w, hh; Client(h, out x, out y, out w, out hh);
    using (Bitmap b = new Bitmap(w, hh)) {
      using (Graphics g = Graphics.FromImage(b)) g.CopyFromScreen(x, y, 0, 0, new Size(w, hh));
      if (width <= 0) { b.Save(path, System.Drawing.Imaging.ImageFormat.Png); return; }
      using (Bitmap s = new Bitmap(b, width, width * hh / w)) s.Save(path, System.Drawing.Imaging.ImageFormat.Png);
    }
  }
  // A dialog of this process: its text, which pressing OK closes.  Null when there is none.
  public static string Box(uint pid, bool close) {
    string text = null;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      StringBuilder c = new StringBuilder(64); GetClassNameW(h, c, 64);
      if (p != pid || c.ToString() != "#32770" || !IsWindowVisible(h)) return true;
      StringBuilder t = new StringBuilder();
      IntPtr ok = IntPtr.Zero;
      EnumChildWindows(h, delegate(IntPtr k, IntPtr m) {
        StringBuilder cc = new StringBuilder(64), s = new StringBuilder(2048);
        GetClassNameW(k, cc, 64); GetWindowTextW(k, s, 2048);
        if (cc.ToString() == "Static") t.Append(s.ToString()).Append(' ');
        if (cc.ToString() == "Button") ok = k;
        return true;
      }, IntPtr.Zero);
      text = t.ToString();
      if (close) {
        if (ok != IntPtr.Zero) PostMessageW(ok, 0xF5, IntPtr.Zero, IntPtr.Zero);   // BM_CLICK
        else PostMessageW(h, 0x10, IntPtr.Zero, IntPtr.Zero);                       // WM_CLOSE
      }
      return false;
    }, IntPtr.Zero);
    return text;
  }
}
"@
[PT]::DpiAware()

$vkNames = @{ ENTER = 0x0D; ESC = 0x1B; SPACE = 0x20; TAB = 0x09; SHIFT = 0x10; CTRL = 0x11; ALT = 0x12; LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28 }
function Get-Vk([string]$n) {
    $u = $n.ToUpper()
    if ($vkNames.ContainsKey($u)) { return $vkNames[$u] }
    if ($u -match '^F(\d+)$') { return 0x6F + [int]$Matches[1] }
    if ($u.Length -eq 1) { return [int][char]$u }
    throw "unknown key $n"
}

function Read-Log([string]$path) {
    if (-not (Test-Path $path)) { return @() }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite,Delete')
    try { $r = New-Object IO.StreamReader($fs); return ($r.ReadToEnd() -split "`r?`n") } finally { $fs.Dispose() }
}

class Stop : Exception { Stop([string]$m) : base($m) {} }

$isDev = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Exe)).Contains('TEST_FATAL')
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
$resultsRoot = Join-Path $build "playtest\results\$stamp"
$run = Join-Path $build 'playtest\run'
$summary = @()

foreach ($file in $chosen) {
    $name = $file.BaseName
    $lines = @(Get-Content $file.FullName | ForEach-Object { ($_ -replace '(^|\s)#.*$', '').Trim() } | Where-Object { $_ })
    if (-not $isDev -and ($lines -contains 'dev')) { $summary += [pscustomobject]@{ Scenario = $name; Result = 'SKIP'; Note = 'needs a dev build' }; continue }

    if (Test-Path $run) { Remove-Item $run -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $run 'savegame'), (Join-Path $run 'cache') | Out-Null
    $runExe = Join-Path $run 'GangstarMV.exe'
    Copy-Item $Exe $runExe
    $patch = Join-Path (Split-Path $Exe) 'gmv_patch.bin'
    if (-not (Test-Path $patch)) { $patch = Join-Path $build 'gmv_patch.bin' }
    Copy-Item $patch $run
    $sdlDll = Join-Path (Split-Path $Exe) 'SDL3.dll'
    if (Test-Path $sdlDll) { Copy-Item $sdlDll $run }
    $res = Join-Path $resultsRoot $name
    New-Item -ItemType Directory -Force $res | Out-Null

    $ini = @(); $allow = @(); $steps = @(); $noIni = $false; $envs = @()
    foreach ($l in $lines) {
        $w = $l -split '\s+', 2
        switch ($w[0]) {
            'ini' { $ini += $w[1] }
            'noini' { $noIni = $true }
            'env' { $envs += $w[1] }
            'dev' {}
            'allow' { $allow += $w[1] }
            'save' {
                Copy-Item -LiteralPath (Join-Path $build "savegame\$($w[1])") (Join-Path $run 'savegame\Gangstar2.sav')
                $pref = Join-Path $build 'savegame\Gangstar2.preferences'
                if (Test-Path $pref) { Copy-Item $pref (Join-Path $run 'savegame') }
            }
            'noprefs' { Remove-Item (Join-Path $run 'savegame\Gangstar2.preferences') -ErrorAction SilentlyContinue }
            'addon' { Copy-Item (Join-Path $build 'add-on') (Join-Path $run 'add-on') -Recurse }
            'songs' {
                $songs = Join-Path $build 'playtest\songs'
                if (-not (Test-Path $songs)) { throw "no $($songs): put a few short songs there first" }
                Copy-Item $songs (Join-Path $run 'custom radio') -Recurse
            }
            default { $steps += $l }
        }
    }
    if (-not $noIni) { [IO.File]::WriteAllLines((Join-Path $run 'savegame\GangstarMV.ini'), [string[]]$ini) }

    Write-Host "--- $name"
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $runExe
    $psi.Arguments = "--app `"$app`""
    $psi.WorkingDirectory = $run
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($e in $envs) { $kv = $e -split '=', 2; $psi.EnvironmentVariables[$kv[0]] = $kv[1] }
    $p = [Diagnostics.Process]::Start($psi)
    $logPath = Join-Path $run 'cache\GangstarMV.log'
    $hwnd = [IntPtr]::Zero
    $failures = New-Object Collections.ArrayList
    $focused = $false; $held = @{}; $heldScan = @{}
    $fail = { param($m) [void]$failures.Add($m); Write-Host "  FAIL: $m" }
    $window = {
        if ($hwnd -eq [IntPtr]::Zero) {
            for ($i = 0; $i -lt 1500 -and $hwnd -eq [IntPtr]::Zero; $i++) {
                if ($p.HasExited) { throw [Stop]::new("the game ended before its window (exit $($p.ExitCode))") }
                if ($i % 10 -eq 0) {
                    $b = [PT]::Box([uint32]$p.Id, $true)
                    if ($b) { throw [Stop]::new("error box: $b") }
                }
                Start-Sleep -Milliseconds 20
                $script:hwnd = [PT]::GameWindow([uint32]$p.Id)
                $hwnd = $script:hwnd
            }
            if ($hwnd -eq [IntPtr]::Zero) { throw [Stop]::new('no window after 30 s') }
        }
        return $hwnd
    }
    $watch = {
        param([switch]$Focus)
        if ($p.HasExited) { throw [Stop]::new("the game ended unexpectedly (exit $($p.ExitCode))") }
        $b = [PT]::Box([uint32]$p.Id, $false)
        if ($b) { [void][PT]::Box([uint32]$p.Id, $true); throw [Stop]::new("error box: $b") }
        $h = & $window
        if ([PT]::Hung($h)) {
            $t = 0
            while ([PT]::Hung($h) -and $t -lt $HangSeconds) { Start-Sleep 1; $t++ }
            if ([PT]::Hung($h)) { throw [Stop]::new("the window did not answer for $HangSeconds s") }
        }
        if ($Focus) {
            if (-not $script:focused -and -not [PT]::TakeFocus($h)) { Start-Sleep -Milliseconds 300 }
            if (-not [PT]::HasFocus($h)) { throw [Stop]::new('FOCUS LOST: another window took the foreground; no more input was sent') }
            $script:focused = $true
        }
        return $h
    }

    try {
        foreach ($s in $steps) {
            if ($s -match '^(dev|release):\s*(.*)$') {
                if (($Matches[1] -eq 'dev') -ne $isDev) { continue }
                $s = $Matches[2]
            }
            $a = $s -split '\s+'
            $rest = ($s -split '\s+', 2)[1]
            Write-Host "  $s"
            switch ($a[0]) {
                'wait' {
                    $end = (Get-Date).AddSeconds([double]$a[1])
                    while ((Get-Date) -lt $end) { [void](& $watch); Start-Sleep -Milliseconds 250 }
                }
                'waitlog' {
                    $tm = 60; $re = $rest
                    if ($rest -match '^(.*\S)\s+(\d+)$') { $re = $Matches[1]; $tm = [int]$Matches[2] }
                    $end = (Get-Date).AddSeconds($tm); $hit = $false
                    while ((Get-Date) -lt $end -and -not $hit) {
                        [void](& $watch)
                        $hit = [bool](Read-Log $logPath | Where-Object { $_ -match $re } | Select-Object -First 1)
                        if (-not $hit) { Start-Sleep -Milliseconds 300 }
                    }
                    if (-not $hit) { throw [Stop]::new("no log line /$re/ in $tm s") }
                }
                { $_ -in 'click', 'rclick', 'move' } {
                    $h = & $watch -Focus
                    [PT]::Point($h, [double]$a[1] - 0.03, [double]$a[2] - 0.03); Start-Sleep -Milliseconds 150
                    [PT]::Point($h, [double]$a[1], [double]$a[2]); Start-Sleep -Milliseconds 200
                    if ($a[0] -ne 'move') {
                        $down, $up = if ($a[0] -eq 'click') { 2, 4 } else { 8, 16 }
                        [PT]::Button($down); Start-Sleep -Milliseconds 120
                        [PT]::Button($up); Start-Sleep -Milliseconds 100
                    }
                }
                'key' {
                    $h = & $watch -Focus
                    $vk = Get-Vk $a[1]; $ms = if ($a.Count -gt 2) { [int]$a[2] } else { 120 }
                    [PT]::Key($vk, $true); $held[$vk] = 1; Start-Sleep -Milliseconds $ms
                    [PT]::Key($vk, $false); $held.Remove($vk)
                }
                'hold' { $h = & $watch -Focus; $vk = Get-Vk $a[1]; [PT]::Key($vk, $true); $held[$vk] = 1 }
                'release' { $vk = Get-Vk $a[1]; [PT]::Key($vk, $false); $held.Remove($vk) }
                'holdscan' { $h = & $watch -Focus; $sc = [Convert]::ToInt32($a[1], 16); [PT]::Scan($sc, $true); $heldScan[$sc] = 1 }
                'releasescan' { $sc = [Convert]::ToInt32($a[1], 16); [PT]::Scan($sc, $false); $heldScan.Remove($sc) }
                'layout' { $h = & $watch; if (-not [PT]::SetLayout($h, $a[1])) { throw [Stop]::new("cannot load keyboard layout $($a[1])") } }
                'look' {
                    $n = if ($a.Count -gt 3) { [int]$a[3] } else { 20 }
                    for ($i = 0; $i -lt $n; $i++) {
                        $h = & $watch -Focus
                        [PT]::Rel([int]([double]$a[1] / $n), [int]([double]$a[2] / $n)); Start-Sleep -Milliseconds 16
                    }
                }
                'drag' {
                    $h = & $watch -Focus
                    $mid = if ($a.Count -gt 3) { Join-Path $res "$($a[3]).png" } else { $null }
                    [PT]::Drag($h, [int]$a[1], [int]$a[2], $mid)
                }
                'size' {
                    $h = & $watch
                    $cx = 0; $cy = 0; $cw = 0; $ch = 0
                    [PT]::Client($h, [ref]$cx, [ref]$cy, [ref]$cw, [ref]$ch)
                    Write-Host "    client ${cw}x${ch}"
                    $px = if ($a.Count -gt 3) { [int]$a[3] } else { 0 }
                    if ([Math]::Abs($cw - [int]$a[1]) -gt $px -or [Math]::Abs($ch - [int]$a[2]) -gt $px) { & $fail "the client area is ${cw}x${ch}, not $($a[1])x$($a[2])" }
                }
                { $_ -in 'minimize', 'unfocus' } {
                    $h = & $watch -Focus
                    $other = $null
                    if ($a[0] -eq 'minimize') { [PT]::SysCommand($h, 0xF020) }
                    else {
                        Add-Type -AssemblyName System.Windows.Forms
                        $other = New-Object System.Windows.Forms.Form
                        $other.Text = 'playtest'; $other.StartPosition = 'Manual'; $other.Left = 0; $other.Top = 0
                        $other.Width = 240; $other.Height = 80; $other.TopMost = $true; $other.ShowInTaskbar = $false
                        $other.Show(); [System.Windows.Forms.Application]::DoEvents()
                        if (-not [PT]::TakeFocus($other.Handle)) { $other.Close(); throw [Stop]::new('could not take the foreground from the game') }
                    }
                    $end = (Get-Date).AddSeconds([double]$a[1]); $c0 = $null
                    while ((Get-Date) -lt $end) {
                        [void](& $watch)
                        if ($other) { [System.Windows.Forms.Application]::DoEvents() }
                        if ($a.Count -gt 2 -and $a[2] -eq 'cpu' -and $null -eq $c0 -and (Get-Date) -gt $end.AddSeconds(2 - [double]$a[1])) {
                            $p.Refresh(); $c0 = $p.TotalProcessorTime.TotalMilliseconds; $t0 = Get-Date
                        }
                        Start-Sleep -Milliseconds 250
                    }
                    if ($null -ne $c0) {
                        $p.Refresh()
                        $pct = ($p.TotalProcessorTime.TotalMilliseconds - $c0) / ((Get-Date) - $t0).TotalMilliseconds * 100
                        Write-Host ('    cpu {0}: {1:N1} % of one core' -f $a[0], $pct)
                    }
                    if ($other) { $other.Close(); $other.Dispose() }
                    if ($a[0] -eq 'minimize') { [PT]::SysCommand($h, 0xF120); Start-Sleep -Milliseconds 500 }
                    if (-not [PT]::TakeFocus($h)) { Start-Sleep -Milliseconds 300; [void][PT]::TakeFocus($h) }
                }
                'cpu' {
                    [void](& $watch)
                    $p.Refresh(); $c0 = $p.TotalProcessorTime.TotalMilliseconds; $t0 = Get-Date
                    $end = $t0.AddSeconds([double]$a[2])
                    while ((Get-Date) -lt $end) { [void](& $watch); Start-Sleep -Milliseconds 250 }
                    $p.Refresh()
                    $pct = ($p.TotalProcessorTime.TotalMilliseconds - $c0) / ((Get-Date) - $t0).TotalMilliseconds * 100
                    Write-Host ('    cpu {0}: {1:N1} % of one core' -f $a[1], $pct)
                }
                'maximize' { $h = & $watch -Focus; [PT]::SysCommand($h, 0xF030) }
                'restore' { $h = & $watch -Focus; [PT]::SysCommand($h, 0xF120) }
                'pad' {
                    [void](& $watch -Focus)
                    [IO.File]::WriteAllText((Join-Path $run 'cache\padtest.txt'), [string]$rest)
                }
                'padfor' {
                    [void](& $watch -Focus)
                    [IO.File]::WriteAllText((Join-Path $run 'cache\padtest.txt'), ($s -split '\s+', 3)[2])
                    Start-Sleep -Milliseconds ([int]$a[1])
                    [IO.File]::WriteAllText((Join-Path $run 'cache\padtest.txt'), '')
                }
                'shot' {
                    $h = & $watch -Focus
                    Start-Sleep -Milliseconds 100
                    [PT]::Shot($h, (Join-Path $res "$($a[1]).png"), $(if ($a.Count -gt 2 -and $a[2] -eq 'full') { 0 } else { 960 }))
                }
                { $_ -in 'moved', 'still' } {
                    $d = [PT]::Apart((Join-Path $res "$($a[1]).png"), (Join-Path $res "$($a[2]).png"))
                    $lim = if ($a.Count -gt 3) { [double]$a[3] } else { 12 }
                    Write-Host ("    apart {0:N1}" -f $d)
                    if ($a[0] -eq 'moved' -and $d -lt $lim) { & $fail ("$($a[1]) and $($a[2]) are alike ({0:N1})" -f $d) }
                    if ($a[0] -eq 'still' -and $d -gt $lim) { & $fail ("$($a[1]) and $($a[2]) differ ({0:N1})" -f $d) }
                }
                'edge' {
                    $d = [PT]::EdgeLeak((Join-Path $res "$($a[1]).png"))
                    $lim = if ($a.Count -gt 2) { [double]$a[2] } else { 3 }
                    Write-Host ("    edge {0:N1}" -f $d)
                    if ($d -gt $lim) { & $fail ("$($a[1]) has a line along its top or left edge ({0:N1})" -f $d) }
                }
                'leftmenu' {
                    $d = [PT]::ApartButtons((Join-Path $res "$($a[1]).png"), (Join-Path $res "$($a[2]).png"))
                    Write-Host ("    buttons apart {0:N1}" -f $d)
                    if ($d -lt 12) { & $fail ("$($a[2]) still shows the main menu ({0:N1})" -f $d) }
                }
                'expectlog' { if (-not (Read-Log $logPath | Where-Object { $_ -match $rest } | Select-Object -First 1)) { & $fail "no log line /$rest/" } }
                'nolog' {
                    $hit = Read-Log $logPath | Where-Object { $_ -match $rest } | Select-Object -First 1
                    if ($hit) { & $fail "log line /$rest/: $hit" }
                }
                'dumpclean' {
                    $bytes = [IO.File]::ReadAllBytes((Join-Path $run 'cache\GangstarMV.dmp'))
                    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
                    foreach ($w in $env:USERNAME, $env:COMPUTERNAME, $env:USERPROFILE, (Split-Path $root -Leaf), (Split-Path (Split-Path $root -Parent) -Leaf)) {
                        foreach ($form in $w, ($w.ToCharArray() -join "`0")) {
                            if ($text.IndexOf($form, [StringComparison]::OrdinalIgnoreCase) -ge 0) { & $fail "the dump still holds $w" }
                        }
                    }
                }
                'expectfile' { if (-not (Test-Path (Join-Path $run $rest))) { & $fail "no file $rest" } }
                'inihas' {
                    $f = Join-Path $run 'savegame\GangstarMV.ini'
                    if (-not ((Test-Path $f) -and (Get-Content $f | Where-Object { $_ -match $rest } | Select-Object -First 1))) { & $fail "no ini line /$rest/" }
                }
                'inisec' {
                    $f = Join-Path $run 'savegame\GangstarMV.ini'
                    $where = @{}; $sec = ''
                    if (Test-Path $f) {
                        foreach ($l in Get-Content $f) {
                            if ($l -match '^\s*\[([^\]]*)\]') { $sec = $Matches[1] }
                            elseif ($l -match '^\s*([A-Za-z0-9_]+)\s*=') { $where[$Matches[1]] = $sec }
                        }
                    }
                    foreach ($k in $a[2..($a.Count - 1)]) {
                        if ($where[$k] -ne $a[1]) { & $fail "ini key $k is in [$($where[$k])], not [$($a[1])]" }
                    }
                }
                'nofile' { if (Test-Path (Join-Path $run $rest)) { & $fail "file $rest should not be there" } }
                'expectbox' {
                    $tm = 60; $re = $rest
                    if ($rest -match '^(.*\S)\s+(\d+)$') { $re = $Matches[1]; $tm = [int]$Matches[2] }
                    $end = (Get-Date).AddSeconds($tm); $b = $null
                    while ((Get-Date) -lt $end -and -not $b -and -not $p.HasExited) { $b = [PT]::Box([uint32]$p.Id, $false); if (-not $b) { Start-Sleep -Milliseconds 200 } }
                    if (-not $b) { throw [Stop]::new("no error box in $tm s") }
                    Set-Content (Join-Path $res 'box.txt') $b
                    if ($b -notmatch $re) { & $fail "the box says: $b" }
                    Start-Sleep -Milliseconds 200
                    [void][PT]::Box([uint32]$p.Id, $true)
                }
                'expectexit' {
                    $tm = if ($a.Count -gt 2) { [int]$a[2] } else { 30 }
                    if (-not $p.WaitForExit($tm * 1000)) { throw [Stop]::new("still running after $tm s") }
                    if ($p.ExitCode -ne [int]$a[1]) { & $fail "exit code $($p.ExitCode), expected $($a[1])" }
                }
                default { throw "scenario $name, unknown step: $s" }
            }
        }
    } catch [Stop] {
        & $fail $_.Exception.Message
    } finally {
        foreach ($vk in @($held.Keys)) { [PT]::Key($vk, $false) }
        foreach ($sc in @($heldScan.Keys)) { [PT]::Scan($sc, $false) }
        [PT]::RestoreLayout($hwnd, -not $p.HasExited)
        if (-not $p.HasExited) {
            if ($hwnd -ne [IntPtr]::Zero -and [PT]::HasFocus($hwnd)) { try { [PT]::Shot($hwnd, (Join-Path $res 'last.png'), 960) } catch {} }
            $p.Kill(); [void]$p.WaitForExit(5000)
        }
    }

    $log = @(Read-Log $logPath)
    foreach ($re in '^\[fault\]', '^\[MISSING', '^\[crash58\]') {
        $hit = $log | Where-Object { $_ -match $re } | Where-Object { $l = $_; -not @($allow | Where-Object { $l -match $_ }).Count } | Select-Object -First 1
        if ($hit) { & $fail "log: $hit" }
    }
    foreach ($f in 'cache\GangstarMV.log', 'cache\GangstarMV.dmp', 'savegame\GangstarMV.ini') {
        $src = Join-Path $run $f
        if (Test-Path $src) { Copy-Item $src $res }
    }
    $state = if ($failures.Count) { 'FAIL' } else { 'PASS' }
    $note = ($failures | Select-Object -First 1)
    Set-Content (Join-Path $res 'result.txt') (@("$name $state") + @($failures))
    Write-Host "  $state"
    $summary += [pscustomobject]@{ Scenario = $name; Result = $state; Note = $note }
    Start-Sleep -Milliseconds 500
}

""
$summary | Format-Table -AutoSize -Wrap | Out-String -Width 160
Get-ChildItem (Split-Path $resultsRoot) -Directory | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Recurse -Force
"results: $resultsRoot"
if (@($summary | Where-Object Result -eq 'FAIL').Count) { exit 1 }
