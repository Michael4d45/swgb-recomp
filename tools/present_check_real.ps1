<#
  The presenter on the real screen: the real mouse, the real window.

    powershell -ExecutionPolicy Bypass -File tools\present_check_real.ps1 [-Yes] [-Quick]

  tools\present_check.py checks the same things headless, with a scripted cursor. This
  is the real thing: it starts the game in a window on your screen, clicks through to a
  skirmish with SendInput, then puts the REAL cursor (SetCursorPos) on every edge of
  the window, in the letterbox bars, past the screen's edge in fullscreen, and compares
  the game's picture before and after (a screen capture of the window). It also checks
  the Windows cursor when the pointer comes back from another place, and reads the
  present log for the frame rate.

  It TAKES OVER THE MOUSE AND KEYBOARD FOR ABOUT FIVE MINUTES (-Quick: three) and puts
  the window in front. Do not touch the mouse or the keyboard until it prints the
  summary. To stop: close the console window (the game is ended with it).

  Each case prints PASS or FAIL; the exit code is 1 on any FAIL.
#>
param(
    [switch] $Yes,                 # skip the countdown
    [switch] $Quick,               # one window shape instead of three, fewer probes
    [string] $Exe  = 'A:\Games\star\builds\presentation\swgbrecomp.exe',
    [string] $Game = 'A:\Games\star\games\presentation',
    [string] $Lift = 'A:\Games\star\swgb-recomp\work\Battlegrounds.exe',
    [string] $Out  = (Join-Path $env:TEMP 'present_check_real')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'game.log'

Add-Type -AssemblyName System.Drawing
$refs = if ($PSVersionTable.PSEdition -eq 'Core') { @('System.Drawing.Common', 'System.Drawing.Primitives', 'System.Runtime.InteropServices', 'System.Runtime') } else { @('System.Drawing') }
Add-Type -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr r);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    // the process's main window: its biggest visible top-level window without an owner
    public static IntPtr MainWindow(int pid) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != (uint)pid || !IsWindowVisible(h) || GetWindow(h, 4) != IntPtr.Zero) return true;   // 4: GW_OWNER
            RECT r; GetWindowRect(h, out r);
            long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (a > area) { area = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool AdjustWindowRect(ref RECT r, uint style, bool menu);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr inst, IntPtr name);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
    // SendInput's INPUT exactly as Windows has it (40 bytes in a 64-bit process, 28 in a 32-bit one)
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
    public static uint Key(ushort vk, bool up) {
        var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.dwFlags = up ? 2u : 0u;
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static uint Click() {
        var i = new INPUT[2]; i[0].type = 0; i[0].u.mi.dwFlags = 2; i[1].type = 0; i[1].u.mi.dwFlags = 4;   // left down, left up
        return SendInput(2, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static int InputSize() { return Marshal.SizeOf(typeof(INPUT)); }
    // the share of pixels that differ (by more than 24 in a channel sum/3) between two screen captures
    public static double Diff(Bitmap a, Bitmap b) {
        if (a.Width != b.Width || a.Height != b.Height) return 1.0;
        var r = new Rectangle(0, 0, a.Width, a.Height);
        var da = a.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var db = b.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        int n = a.Width * a.Height; byte[] x = new byte[n * 4], y = new byte[n * 4];
        Marshal.Copy(da.Scan0, x, 0, n * 4); Marshal.Copy(db.Scan0, y, 0, n * 4);
        a.UnlockBits(da); b.UnlockBits(db);
        long d = 0;
        for (int i = 0; i < n; i++) {
            int s = Math.Abs(x[i*4] - y[i*4]) + Math.Abs(x[i*4+1] - y[i*4+1]) + Math.Abs(x[i*4+2] - y[i*4+2]);
            if (s > 72) d++;
        }
        return (double)d / n;
    }
}
'@
[void][W]::SetProcessDPIAware()

$script:fail = 0
$script:pass = 0
function Check([string] $name, [bool] $ok, [string] $detail = '') {
    if ($ok) { $script:pass++ } else { $script:fail++ }
    Write-Host ("{0}: {1}  ({2})" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# ------------------------------------------------------------------ the game
if (-not $Yes) {
    Write-Host "This takes over your mouse and keyboard for about $(if ($Quick) {3} else {5}) minutes. Starting in 10 s: Ctrl+C now to cancel."
    Start-Sleep 10
}
if (Get-Process swgbrecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }) {
    throw "a game from $Exe is already running"
}
$orig = New-Object W+POINT; [void][W]::GetCursorPos([ref]$orig)
$sw = [Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -PassThru -FilePath $Exe -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
    -ArgumentList @('--run', '--multi', '--exe', "`"$Lift`"", '--game', "`"$Game`"", '--present-log')

function Log-Text { (Get-Content $log -Raw -ErrorAction SilentlyContinue) + (Get-Content "$log.err" -Raw -ErrorAction SilentlyContinue) }
function Sleep-Until([double] $seconds) { $d = $seconds - $sw.Elapsed.TotalSeconds; if ($d -gt 0) { Start-Sleep -Milliseconds ([int]($d * 1000)) } }

function Window { [W]::MainWindow($proc.Id) }   # by the process, not the title
function Client {   # the window's client rectangle on the screen, and the game's own size
    $h = Window
    if ($h -eq [IntPtr]::Zero) { throw 'the game window is gone' }
    $r = New-Object W+RECT; [void][W]::GetClientRect($h, [ref]$r)
    $o = New-Object W+POINT; [void][W]::ClientToScreen($h, [ref]$o)
    $m = [regex]::Matches((Log-Text), 'SetDisplayMode\((\d+), (\d+)')
    $gw = 800; $gh = 600
    if ($m.Count -gt 0) { $gw = [int]$m[$m.Count - 1].Groups[1].Value; $gh = [int]$m[$m.Count - 1].Groups[2].Value }
    [pscustomobject]@{ H = $h; X = $o.X; Y = $o.Y; W = $r.Right; Ht = $r.Bottom; GW = $gw; GH = $gh }
}
function Picture($c) {   # the picture's rectangle in the client area (sharp/smooth/nearest: a fit)
    if ($c.W * $c.GH -le $c.Ht * $c.GW) { $dw = $c.W; $dh = [int]($c.W * $c.GH / $c.GW) } else { $dh = $c.Ht; $dw = [int]($c.Ht * $c.GW / $c.GH) }
    [pscustomobject]@{ L = [int](($c.W - $dw) / 2); T = [int](($c.Ht - $dh) / 2); W = $dw; H = $dh }
}
function Game-To-Screen([int] $gx, [int] $gy) {
    $c = Client; $p = Picture $c
    @([int]($c.X + $p.L + ($gx + 0.5) * $p.W / $c.GW), [int]($c.Y + $p.T + ($gy + 0.5) * $p.H / $c.GH))
}
function Mouse([int] $x, [int] $y) { [void][W]::SetCursorPos($x, $y) }
function Click-Game([int] $gx, [int] $gy) {
    $s = Game-To-Screen $gx $gy; Mouse $s[0] $s[1]; Start-Sleep -Milliseconds 300
    if ([W]::Click() -ne 2) { throw 'SendInput refused the click (is an elevated window in front?)' }
    Start-Sleep -Milliseconds 400
}
function Press([int] $vk) {
    if ([W]::Key($vk, $false) -ne 1) { throw 'SendInput refused a key' }
    Start-Sleep -Milliseconds 80; [void][W]::Key($vk, $true); Start-Sleep -Milliseconds 200
}
function Present-Lines {   # the [present] lines of the log so far: elapsed, presents, shown, Hz
    [regex]::Matches((Log-Text), '\[present\] ([\d.]+) s: (\d+) presents, (\d+) shown \((\d+) Hz') | ForEach-Object {
        [pscustomobject]@{ El = [double]$_.Groups[1].Value; P = [int]$_.Groups[2].Value; S = [int]$_.Groups[3].Value; Hz = [int]$_.Groups[4].Value } }
}
function Front { [void][W]::SetForegroundWindow((Window)); Start-Sleep -Milliseconds 300 }
function Grab($c, [string] $name) {   # the client area, as it is on the screen
    $b = New-Object Drawing.Bitmap $c.W, $c.Ht
    $g = [Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($c.X, $c.Y, 0, 0, $b.Size)
    $g.Dispose()
    $b.Save((Join-Path $Out "$name.png"), [Drawing.Imaging.ImageFormat]::Png)
    $b
}

try {
    # wait for the window and the main menu
    while ((Window) -eq [IntPtr]::Zero) {
        if ($proc.HasExited) { throw "the game ended (code $($proc.ExitCode)); its log: $log" }
        if ($sw.Elapsed.TotalSeconds -gt 60) { throw "no game window after 60 s; its log: $log" }
        Start-Sleep -Milliseconds 500
    }
    Write-Host 'the game window is up; waiting for the main menu...'
    Sleep-Until 14
    Front
    Click-Game 345 250        # Single Player
    Start-Sleep -Seconds 2
    Click-Game 600 168        # Standard Game
    Start-Sleep -Seconds 3
    Click-Game 118 568        # Start Game
    # the map is up when the game presents steadily (in a game: hundreds a second; loading: tens of
    # thousands; the menus: 4 or 5) for 6 seconds in a row
    Write-Host 'waiting for the map...'
    $from = @(Present-Lines).Count
    while ($true) {
        $l = @(Present-Lines) | Select-Object -Skip $from
        $run = 0; foreach ($x in $l) { $r = $x.P / $x.El; if ($r -ge 40 -and $r -le 20000) { $run++ } else { $run = 0 } }
        if ($run -ge 6) { break }
        if ($proc.HasExited) { throw "the game ended (code $($proc.ExitCode)); its log: $log" }
        if ($sw.Elapsed.TotalSeconds -gt 200) { throw "the map did not come up (no steady game frames in the log); its log: $log" }
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Seconds 2
    Write-Host 'the map is up'
    Front
    Mouse ((Client).X + 300) ((Client).Y + 300); Start-Sleep -Seconds 1

    $arrow = [W]::LoadCursor([IntPtr]::Zero, [IntPtr]32512)
    $shapes = @(@{ n = 'the window as it started'; w = 0; h = 0 })
    if (-not $Quick) { $shapes += @{ n = 'a wide window'; w = 1500; h = 700 }, @{ n = 'a tall window'; w = 700; h = 900 } }

    function Probe($c, [string] $label, [double] $px, [double] $py, [bool] $expectScroll, [string] $tag) {
        # px, py: a place in the client area (fractions: 0 = the edge pixel, 1 = the far edge, 0.5 the middle); past the edge: -0.2 / 1.2
        $x = $c.X + [int]($px * ($c.W - 1)); $y = $c.Y + [int]($py * ($c.Ht - 1))
        Press 0x48                                         # H: the view on the command center
        Mouse ($c.X + [int]($c.W / 2)) ($c.Y + [int]($c.Ht / 2)); Start-Sleep -Milliseconds 900
        $a = Grab $c "$tag-a"
        Mouse $x $y; Start-Sleep -Milliseconds 800
        Mouse ($c.X + [int]($c.W / 2)) ($c.Y + [int]($c.Ht / 2)); Start-Sleep -Milliseconds 900
        $b = Grab $c "$tag-b"
        $d = [W]::Diff($a, $b); $a.Dispose(); $b.Dispose()
        if ($expectScroll) { Check "$label scrolls" ($d -ge 0.10) ("the view changed {0:P1}" -f $d) }
        else { Check "$label does not scroll" ($d -lt 0.03) ("the view changed {0:P1}" -f $d) }
    }

    function Battery([string] $name, [bool] $fullscreen) {
        Front
        $c = Client; $p = Picture $c
        Write-Host ("--- {0}: client {1}x{2}, picture {3},{4} {5}x{6}" -f $name, $c.W, $c.Ht, $p.L, $p.T, $p.W, $p.H)
        $t = ($name -replace '[^A-Za-z0-9]+', '_')
        # a point 60 px inside the picture's left/top edge, and the middle: nothing scrolls
        Probe $c "$name / well inside the left edge" (($p.L + 60) / [double]($c.W - 1)) 0.5 $false "$t-in-left"
        Probe $c "$name / well inside the top edge" 0.5 (($p.T + 60) / [double]($c.Ht - 1)) $false "$t-in-top"
        Probe $c "$name / the middle" 0.5 0.5 $false "$t-mid"
        Probe $c "$name / right edge" 1.0 0.5 $true "$t-r"
        Probe $c "$name / left edge" 0.0 0.5 $true "$t-l"
        Probe $c "$name / bottom edge" 0.5 1.0 $true "$t-b"
        Probe $c "$name / top edge" 0.5 0.0 $true "$t-t"
        if ($fullscreen) {   # the cursor cannot go past the screen: pushing at it is the same as the edge
            Mouse ($c.X + $c.W + 400) ($c.Y + 200)
            $q = New-Object W+POINT; [void][W]::GetCursorPos([ref]$q)
            Check "$name / the cursor stays on the screen when pushed past its right edge" ($q.X -le $c.X + $c.W - 1) "cursor x $($q.X), the window's right $($c.X + $c.W - 1)"
        }
        if ($p.L -ge 8) {
            Probe $c "$name / right bar" (($p.L + $p.W + $c.W) / 2 / [double]($c.W - 1)) 0.5 $true "$t-rb"
            Probe $c "$name / left bar" (($p.L / 2) / [double]($c.W - 1)) 0.5 $true "$t-lb"
        }
        if ($p.T -ge 8) {
            Probe $c "$name / bottom bar" 0.5 (($p.T + $p.H + $c.Ht) / 2 / [double]($c.Ht - 1)) $true "$t-bb"
            Probe $c "$name / top bar" 0.5 (($p.T / 2) / [double]($c.Ht - 1)) $true "$t-tb"
        }
    }

    foreach ($s in $shapes) {
        if ($s.w -gt 0) {
            $h = Window; $c = Client
            $r = New-Object W+RECT; $r.Right = $s.w; $r.Bottom = $s.h
            [void][W]::AdjustWindowRect([ref]$r, [uint32][W]::GetWindowLong($h, -16), $false)
            [void][W]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, $r.Right - $r.Left, $r.Bottom - $r.Top, 0x4)   # no z-order change
            Start-Sleep -Milliseconds 1500
        }
        Battery $s.n $false
    }

    # the cursor when the pointer comes back: park it on the desktop (the top-left corner of the screen,
    # outside the game window), then in again over the picture, and look at what Windows shows
    $c = Client
    Mouse 2 2; Start-Sleep -Milliseconds 700
    Mouse ($c.X + [int]($c.W / 2)) ($c.Y + [int]($c.Ht / 2)); Start-Sleep -Milliseconds 700
    $ci = New-Object W+CURSORINFO; $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci); [void][W]::GetCursorInfo([ref]$ci)
    $showing = ($ci.flags -band 1) -ne 0
    Check 'the Windows arrow is not shown over the game when the pointer comes back' (-not ($showing -and $ci.hCursor -eq $arrow)) "showing $showing, cursor $($ci.hCursor), the arrow $arrow"

    if (-not $Quick) {
        Write-Host '--- Alt+Enter: borderless fullscreen'
        [W]::Key(0x12, $false); Press 0x0D; [W]::Key(0x12, $true); Start-Sleep -Seconds 2
        Battery 'fullscreen' $true
        Write-Host '--- the Options size change while fullscreen is checked by the headless tool (present_check.py B)'
        [W]::Key(0x12, $false); Press 0x0D; [W]::Key(0x12, $true); Start-Sleep -Seconds 1
    }

    # the frame rate: from the game's own log, while the skirmish ran
    # (presents above the refresh but below loading's tens of thousands: the game itself)
    $lines = @(Present-Lines)
    $busy = @($lines | Where-Object { $_.P -gt 1.05 * $_.Hz * $_.El -and $_.P -lt 20000 * $_.El })
    if ($busy.Count -ge 5) {
        $el = ($busy | Measure-Object -Property El -Sum).Sum
        $shown = ($busy | Measure-Object -Property S -Sum).Sum
        $pres = ($busy | Measure-Object -Property P -Sum).Sum
        $hz = $busy[0].Hz
        Check 'frames shown at about the refresh rate when presents exceed it' (($shown / $el) -ge 0.85 * $hz) ("{0:N0} presents/s, {1:N0} shown/s at {2} Hz over {3:N0} s" -f ($pres / $el), ($shown / $el), $hz, $el)
    } else { Write-Host 'INFO: the game never presented faster than the refresh; no frame-rate case' }
    $idle = @($lines | Where-Object { $_.P -le 8 })
    if ($idle.Count -ge 5) {
        $extra = ($idle | ForEach-Object { $_.S - $_.P } | Measure-Object -Average).Average
        Check 'an idle menu shows no more frames than it presents' ($extra -le 2.5) ("{0:N1} more shown than presented a second (the log's timer columns say why if not 0)" -f $extra)
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    [void][W]::ClipCursor([IntPtr]::Zero)   # fullscreen confines the cursor: always let it go
    [void][W]::SetCursorPos($orig.X, $orig.Y)
}
Write-Host ("`n{0}/{1} passed" -f $script:pass, ($script:pass + $script:fail))
Write-Host "Captures and the game's log: $Out"
if ($script:fail) { exit 1 }
