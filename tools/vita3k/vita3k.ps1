<#
.SYNOPSIS
  Headless Vita3K test harness: install a VPK, run it by TITLE ID, grab a
  960x544 PNG of the emulated screen, stop the emulator. Nothing is shown on
  the user's screen: Vita3K runs on a separate hidden Win32 desktop.

.DESCRIPTION
  Subcommands
    setup [-Update]                 Download Vita3K + firmware if missing, install
                                    firmware, write a portable config. Idempotent.
    install <file.vpk>              Install a VPK (Vita3K CLI install, then stop).
                                    Prints the installed TITLE ID.
    run <TITLEID> [-Seconds N]      Boot an installed app on the hidden desktop,
                                    wait for its window, then N more seconds
                                    (default 5). The emulator keeps running.
    shot <out.png>                  Save the current emulated frame as PNG.
    stop                            Close Vita3K (WM_CLOSE, then kill after 15 s).
    status                          Show the running instance and its windows.
    smoke <file.vpk> <out.png>      install + run + shot + stop in one call.

  Common options
    -Renderer Vulkan|OpenGL         Vita3K backend (default Vulkan).
    -PrintWindow                    shot: capture the window with PrintWindow
                                    instead of the native screenshot (diagnosis).
    -Root <dir>                     Install root (default .vita3k,
                                    or $env:VITA3K_ROOT).

  How it works
    * Vita3K lives in <Root>/bin with a "portable" folder next to the exe, so
      config, ux0, logs and screenshots all stay in <Root>/bin/portable.
    * The process is created with CreateProcess, STARTUPINFO.lpDesktop set to
      a desktop made with CreateDesktop ("vita3k-harness"), BelowNormal
      priority, affinity 0xAAAA (secondary SMT threads), SDL_AUDIO_DRIVER=dummy
      and audio-volume 0.
    * "shot" uses Vita3K's own screenshot hotkey (bound to F9 in config): a
      WM_KEYDOWN/WM_KEYUP pair is posted to the game window from a thread
      attached to the hidden desktop, Vita3K reads the guest frame back from
      the renderer and writes a PNG. -PrintWindow captures the window instead
      (blank on the hidden desktop, kept for diagnosis only).
    * Use the Vulkan backend: with OpenGL the native screenshot is all black.
    * Run state (pid) is kept in <Root>/run/state.json.

.EXAMPLE
  pwsh -File vita3k.ps1 setup
  pwsh -File vita3k.ps1 smoke D:/x/app.vpk D:/x/shot.png
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('setup', 'install', 'run', 'shot', 'stop', 'status', 'smoke')]
    [string]$Command,
    [Parameter(Position = 1)] [string]$Arg1,
    [Parameter(Position = 2)] [string]$Arg2,
    [int]$Seconds = 5,
    [ValidateSet('Vulkan', 'OpenGL')] [string]$Renderer = 'Vulkan',
    [string]$Root = $(if ($env:VITA3K_ROOT) { $env:VITA3K_ROOT } else { '.vita3k' }),
    [int]$BootTimeout = 90,
    [int]$LogLimitMB = 256,
    [switch]$Update,
    [switch]$PrintWindow
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

# ---------------------------------------------------------------- paths / constants
$Bin       = Join-Path $Root 'bin'
$Exe       = Join-Path $Bin 'Vita3K.exe'
$Portable  = Join-Path $Bin 'portable'
$FwDir     = Join-Path $Root 'firmware'
$RunDir    = Join-Path $Root 'run'
$StateFile = Join-Path $RunDir 'state.json'
$LogFile   = Join-Path $Portable 'vita3k.log'
$Config    = Join-Path $Portable 'config.yml'
$DesktopName = 'vita3k-harness'
$Affinity  = 0xAAAA

$Vita3KZipUrl = 'https://github.com/Vita3K/Vita3K/releases/download/continuous/windows-latest.zip'
# Official Sony update servers (linked from vita3k.org/quickstart and playstation.com).
$FirmwareUrl  = 'http://dus01.psv.update.playstation.net/update/psv/image/2022_0209/rel_f2c7b12fe85496ec88a0391b514d6e3b/PSVUPDAT.PUP?dest=us'
$FontPupUrl   = 'http://dus01.psp2.update.playstation.net/update/psp2/image/2019_0924/sd_8b5f60b56c3da8365b973dba570c53a5/PSP2UPDAT.PUP?dest=us'

# Screenshot hotkey: F9 (Windows virtual key 0x78, scan code 0x43).
$ShotKeyName = 'F9'; $ShotVk = 0x78; $ShotScan = 0x43

# ---------------------------------------------------------------- Win32 helper
if (-not ('Vita3KHarness.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Vita3KHarness {
public static class Native {
    const uint GENERIC_ALL = 0x10000000;
    const uint CREATE_SUSPENDED = 0x4, CREATE_UNICODE_ENVIRONMENT = 0x400,
               BELOW_NORMAL_PRIORITY_CLASS = 0x4000, CREATE_NO_WINDOW = 0x08000000;
    const int STARTF_USESHOWWINDOW = 1; const short SW_SHOWNOACTIVATE = 4;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr mode, int flags, uint access, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr OpenDesktopW(string name, int flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)] static extern bool SetThreadDesktop(IntPtr h);
    [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetProcessAffinityMask(IntPtr h, UIntPtr mask);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    public static IntPtr OpenOrCreateDesktop(string name) {
        IntPtr h = OpenDesktopW(name, 0, false, GENERIC_ALL);
        if (h == IntPtr.Zero) h = CreateDesktopW(name, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (h == IntPtr.Zero) throw new Exception("CreateDesktop failed" + ": win32 error " + Marshal.GetLastWin32Error());
        return h;
    }
    public static IntPtr TryOpenDesktop(string name) { return OpenDesktopW(name, 0, false, GENERIC_ALL); }
    public static void Close(IntPtr desk) { if (desk != IntPtr.Zero) CloseDesktop(desk); }

    // Start exe on the given desktop: suspended, BelowNormal, affinity set, then resumed.
    public static int Launch(string exe, string args, string cwd, string desktop, ulong affinity) {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = desktop;
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION pi;
        var cmd = new StringBuilder("\"" + exe + "\" " + args);
        uint flags = CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
        if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, flags, IntPtr.Zero, cwd, ref si, out pi))
            throw new Exception("CreateProcess failed" + ": win32 error " + Marshal.GetLastWin32Error());
        try {
            if (!SetProcessAffinityMask(pi.hProcess, new UIntPtr(affinity)))
                throw new Exception("SetProcessAffinityMask failed" + ": win32 error " + Marshal.GetLastWin32Error());
            ResumeThread(pi.hThread);
        } finally { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return pi.dwProcessId;
    }

    public class Win { public long Hwnd; public string Title; public string Class; public bool Visible; public int W; public int H; }

    // Top-level windows of pid on desktop (all pids if pid == 0).
    public static List<Win> Windows(IntPtr desk, int pid) {
        var list = new List<Win>();
        EnumDesktopWindows(desk, (h, l) => {
            int p; GetWindowThreadProcessId(h, out p);
            if (pid == 0 || p == pid) {
                var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
                var c = new StringBuilder(256); GetClassNameW(h, c, 256);
                RECT r; GetClientRect(h, out r);
                list.Add(new Win { Hwnd = h.ToInt64(), Title = t.ToString(), Class = c.ToString(),
                    Visible = IsWindowVisible(h), W = r.Right - r.Left, H = r.Bottom - r.Top });
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }

    // Window handles on another desktop are invalid (error 1400) for threads of the
    // caller's desktop, so messages are posted from a thread attached to the target desktop.
    // msgs is a flat list of (msg, wParam, lParam) triples, delayMs between them.
    public static string Post(string desktop, long hwnd, long[] msgs, int delayMs) {
        string err = null;
        var t = new Thread(() => {
            IntPtr d = OpenDesktopW(desktop, 0, false, GENERIC_ALL);
            if (d == IntPtr.Zero) { err = "OpenDesktop failed: " + Marshal.GetLastWin32Error(); return; }
            try {
                if (!SetThreadDesktop(d)) { err = "SetThreadDesktop failed: " + Marshal.GetLastWin32Error(); return; }
                for (int i = 0; i + 2 < msgs.Length; i += 3) {
                    if (i > 0 && delayMs > 0) Thread.Sleep(delayMs);
                    if (!PostMessageW(new IntPtr(hwnd), (uint)msgs[i], new IntPtr(msgs[i + 1]), new IntPtr(msgs[i + 2]))) {
                        err = "PostMessage failed: " + Marshal.GetLastWin32Error(); return;
                    }
                }
            } finally { CloseDesktop(d); }
        });
        t.Start(); t.Join();
        return err;
    }

    // PrintWindow(PW_RENDERFULLCONTENT) from a worker thread attached to the desktop.
    public static string Capture(string desktop, long hwnd, string outPng) {
        string err = null;
        var t = new Thread(() => {
            IntPtr d = OpenDesktopW(desktop, 0, false, GENERIC_ALL);
            if (d == IntPtr.Zero) { err = "OpenDesktop failed"; return; }
            try {
                if (!SetThreadDesktop(d)) { err = "SetThreadDesktop failed: " + Marshal.GetLastWin32Error(); return; }
                RECT r; GetClientRect(new IntPtr(hwnd), out r);
                int w = r.Right - r.Left, h = r.Bottom - r.Top;
                if (w <= 0 || h <= 0) { err = "empty client rect"; return; }
                using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
                    using (var g = Graphics.FromImage(bmp)) {
                        IntPtr hdc = g.GetHdc();
                        bool ok = PrintWindow(new IntPtr(hwnd), hdc, 1 | 2); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
                        g.ReleaseHdc(hdc);
                        if (!ok) { err = "PrintWindow failed"; return; }
                    }
                    bmp.Save(outPng, ImageFormat.Png);
                }
            } finally { CloseDesktop(d); }
        });
        t.Start(); t.Join();
        return err;
    }
}
}
'@ -ReferencedAssemblies System.Runtime, System.Collections, System.Threading, System.Threading.Thread, System.Runtime.InteropServices, System.Drawing.Common, System.Drawing.Primitives, System.Private.Windows.Core, System.Private.Windows.GdiPlus -WarningAction SilentlyContinue
}

# ---------------------------------------------------------------- small helpers
function Write-Step([string]$msg) { Write-Host "[vita3k] $msg" }

function Read-Shared([string]$path) {
    # Vita3K keeps its log open; read it with a shared handle.
    if (-not (Test-Path $path)) { return '' }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
}

function Get-PngSize([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 24 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { throw "not a PNG: $path" }
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    return [pscustomobject]@{ Width = $w; Height = $h }
}

function Set-ConfigValues([hashtable]$values) {
    # Flat "key: value" lines in Vita3K's config.yml; replace or append.
    $lines = [Collections.Generic.List[string]]::new()
    if (Test-Path $Config) { $lines.AddRange([string[]](Get-Content $Config)) }
    foreach ($k in $values.Keys) {
        $line = "${k}: $($values[$k])"
        $idx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$([regex]::Escape($k)):") { $idx = $i; break } }
        if ($idx -ge 0) { $lines[$idx] = $line }
        else {
            $end = $lines.IndexOf('...')
            if ($end -ge 0) { $lines.Insert($end, $line) } else { $lines.Add($line) }
        }
    }
    if ($lines.Count -eq 0 -or $lines[0] -ne '---') { $lines.Insert(0, '---') }
    Set-Content -Path $Config -Value $lines -Encoding utf8NoBOM
}

function Get-State {
    if (-not (Test-Path $StateFile)) { return $null }
    $s = Get-Content $StateFile -Raw | ConvertFrom-Json
    $p = Get-Process -Id $s.Pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -ne 'Vita3K') { Remove-Item $StateFile -Force; return $null }
    return $s
}

function Get-Vita3KWindows([int]$procId) {
    $desk = [Vita3KHarness.Native]::TryOpenDesktop($DesktopName)
    if ($desk -eq [IntPtr]::Zero) { return @() }
    try { return @([Vita3KHarness.Native]::Windows($desk, $procId)) } finally { [Vita3KHarness.Native]::Close($desk) }
}

function Start-Vita3K([string]$cliArgs, [string]$titleId) {
    if (Get-State) { throw "Vita3K is already running (pid $((Get-State).Pid)); run 'stop' first." }
    if (-not (Test-Path $Exe)) { throw "Vita3K missing at $Exe; run 'setup' first." }
    New-Item -ItemType Directory -Force $RunDir | Out-Null
    # Audio: SDL dummy driver (Vita3K has no null backend; config volume is 0 too).
    $env:SDL_AUDIO_DRIVER = 'dummy'
    $desk = [Vita3KHarness.Native]::OpenOrCreateDesktop($DesktopName)
    $vpid = [Vita3KHarness.Native]::Launch($Exe, $cliArgs, $Bin, $DesktopName, [uint64]$Affinity)
    @{ Pid = $vpid; Desktop = $DesktopName; TitleId = $titleId; Started = (Get-Date).ToString('o'); Args = $cliArgs } |
        ConvertTo-Json | Set-Content $StateFile -Encoding utf8NoBOM
    Write-Step "started pid $vpid on desktop '$DesktopName' ($cliArgs)"
    Start-LogGuard $vpid
    return [pscustomobject]@{ Pid = $vpid; Desk = $desk }
}

# A guest stuck in a faulting loop makes Vita3K log the same exception forever
# (30 GB in three minutes, seen with a NULL ldrex). A detached watcher on the
# hidden desktop kills the emulator past $LogLimitMB and trims the log to that
# size, which keeps the first faults, the ones worth reading.
function Start-LogGuard([int]$vpid) {
    $limit = [int64]$LogLimitMB * 1MB
    $marker = Join-Path $RunDir 'log-overflow.txt'
    Remove-Item $marker -ErrorAction SilentlyContinue
    $script = @"
`$p = Get-Process -Id $vpid -ErrorAction SilentlyContinue
while (`$p -and -not `$p.HasExited) {
    `$l = Get-Item -LiteralPath '$LogFile' -ErrorAction SilentlyContinue
    if (`$l -and `$l.Length -gt $limit) {
        Stop-Process -Id $vpid -Force -ErrorAction SilentlyContinue
        `$p.WaitForExit(10000) | Out-Null
        `$fs = [IO.File]::Open('$LogFile', 'Open', 'ReadWrite', 'ReadWrite')
        try { `$fs.SetLength($limit) } finally { `$fs.Close() }
        Set-Content -LiteralPath '$marker' -Value "killed pid $vpid at `$(Get-Date -Format o): log over $LogLimitMB MB"
        break
    }
    Start-Sleep -Seconds 2
}
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    $pwsh = (Get-Process -Id $PID).Path
    [Vita3KHarness.Native]::Launch($pwsh, "-NoProfile -NonInteractive -EncodedCommand $enc", $RunDir, $DesktopName, [uint64]$Affinity) | Out-Null
}

function Wait-Until([scriptblock]$cond, [int]$timeoutSec, [string]$what, [int]$procId = 0) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        $r = & $cond
        if ($r) { return $r }
        if ($procId -and -not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) {
            throw "Vita3K exited while waiting for $what. Log tail:`n$((Read-Shared $LogFile) -split "`n" | Select-Object -Last 15 | Out-String)"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "timeout ($timeoutSec s) waiting for $what"
}

function Get-GameWindow([int]$procId, [string]$titleId) {
    Get-Vita3KWindows $procId | Where-Object { $_.Title -like "*($titleId)*" -and $_.W -gt 0 } | Select-Object -First 1
}

# ---------------------------------------------------------------- subcommands
function Invoke-Setup {
    New-Item -ItemType Directory -Force $Root, $FwDir, $RunDir | Out-Null
    if ($Update -or -not (Test-Path $Exe)) {
        $zip = Join-Path $RunDir 'windows-latest.zip'
        Write-Step "downloading $Vita3KZipUrl"
        Invoke-WebRequest -Uri $Vita3KZipUrl -OutFile $zip
        Expand-Archive -Path $zip -DestinationPath $Bin -Force
        Remove-Item $zip -Force
    }
    New-Item -ItemType Directory -Force $Portable | Out-Null   # enables Vita3K portable mode
    foreach ($fw in @(@{ Name = 'PSVUPDAT.PUP'; Url = $FirmwareUrl; Dir = 'vs0' }, @{ Name = 'PSP2UPDAT.PUP'; Url = $FontPupUrl; Dir = 'sa0' })) {
        $pup = Join-Path $FwDir $fw.Name
        if (-not (Test-Path $pup)) { Write-Step "downloading $($fw.Name)"; Invoke-WebRequest -Uri $fw.Url -OutFile $pup }
        $target = Join-Path $Portable "fs/$($fw.Dir)"
        if (-not (Test-Path $target) -or -not (Get-ChildItem $target -Recurse -File | Select-Object -First 1)) {
            # --firmware installs and quits without opening a window. The very first run in a
            # fresh portable dir can die in logging init, so try twice.
            for ($try = 1; $try -le 2; $try++) {
                Write-Step "installing $($fw.Name) (try $try)"
                $desk = [Vita3KHarness.Native]::OpenOrCreateDesktop($DesktopName)
                try {
                    $fpid = [Vita3KHarness.Native]::Launch($Exe, "--firmware `"$pup`"", $Bin, $DesktopName, [uint64]$Affinity)
                    $p = Get-Process -Id $fpid -ErrorAction SilentlyContinue
                    if ($p -and -not $p.WaitForExit(300000)) { $p.Kill(); throw 'firmware install timed out' }
                } finally { [Vita3KHarness.Native]::Close($desk) }
                if ((Test-Path $target) -and (Get-ChildItem $target -Recurse -File | Select-Object -First 1)) { break }
                if ($try -eq 2) { throw "firmware install failed; see $LogFile" }
            }
        }
    }
    Set-ConfigValues @{
        'show-welcome'            = 'false'
        'initial-setup'           = 'true'
        'warn-missing-firmware'   = 'false'
        'check-for-updates-mode'  = '0'
        'discord-rich-presence'   = 'false'
        'audio-backend'           = 'SDL'
        'audio-volume'            = '0'
        'validation-layer'        = 'false'
        'v-sync'                  = 'false'
        'boot-apps-full-screen'   = 'false'
        'show-live-area-screen'   = 'false'
        'log-level'               = '2'
        'screenshot-format'       = '2'
        'keyboard-take-screenshot' = $ShotKeyName
        'backend-renderer'        = $Renderer
        'psn-signed-in'           = '0'
    }
    # Qt GUI settings: never ask "exit app?" so WM_CLOSE stops cleanly.
    $gui = Join-Path $Portable 'gui-configs'
    New-Item -ItemType Directory -Force $gui | Out-Null
    $ini = Join-Path $gui 'CurrentSettings.ini'
    $iniText = if (Test-Path $ini) { Get-Content $ini -Raw } else { '' }
    if ($iniText -notmatch 'confirmExitApp=false') {
        if ($iniText -match '(?m)^\[MainWindow\]') { $iniText = $iniText -replace '(?m)^\[MainWindow\]\r?\n', "[MainWindow]`r`nconfirmExitApp=false`r`n" }
        else { $iniText = $iniText.TrimEnd() + "`r`n[MainWindow]`r`nconfirmExitApp=false`r`n" }
        Set-Content -Path $ini -Value $iniText.Trim() -Encoding utf8NoBOM
    }
    Write-Step "setup done: $Exe (portable at $Portable)"
    & $Exe --version 2>$null | Select-Object -First 1 | ForEach-Object { Write-Step $_ }
}

function Get-VpkInfo([string]$vpk) {
    # Read TITLE_ID from sce_sys/param.sfo inside the VPK (a zip) and the eboot.bin size.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($vpk)
    try {
        $sfoEntry = $zip.GetEntry('sce_sys/param.sfo'); $eboot = $zip.GetEntry('eboot.bin')
        if (-not $sfoEntry -or -not $eboot) { throw "not an app VPK (no sce_sys/param.sfo or eboot.bin): $vpk" }
        $ms = New-Object IO.MemoryStream; $st = $sfoEntry.Open(); $st.CopyTo($ms); $st.Dispose()
        $b = $ms.ToArray()
    } finally { $zip.Dispose() }
    if ([BitConverter]::ToUInt32($b, 0) -ne 0x46535000) { throw 'bad param.sfo magic' }
    $keyTable = [BitConverter]::ToUInt32($b, 8); $dataTable = [BitConverter]::ToUInt32($b, 12)
    $count = [BitConverter]::ToUInt32($b, 16)
    for ($i = 0; $i -lt $count; $i++) {
        $e = 20 + 16 * $i
        $kOff = [BitConverter]::ToUInt16($b, $e); $len = [BitConverter]::ToUInt32($b, $e + 4)
        $dOff = [BitConverter]::ToUInt32($b, $e + 12)
        $kStart = $keyTable + $kOff; $kEnd = [Array]::IndexOf($b, [byte]0, [int]$kStart)
        $key = [Text.Encoding]::ASCII.GetString($b, $kStart, $kEnd - $kStart)
        if ($key -eq 'TITLE_ID') {
            $tid = [Text.Encoding]::ASCII.GetString($b, $dataTable + $dOff, $len).TrimEnd([char]0)
            return [pscustomobject]@{ TitleId = $tid; EbootSize = $eboot.Length }
        }
    }
    throw 'TITLE_ID not found in param.sfo'
}

function Invoke-Install([string]$vpk) {
    if (-not $vpk -or -not (Test-Path $vpk)) { throw "install: VPK not found: $vpk" }
    $vpk = (Resolve-Path $vpk).Path
    $info = Get-VpkInfo $vpk
    $eboot = Join-Path $Portable "fs/ux0/app/$($info.TitleId)/eboot.bin"
    # "Vita3K.exe <file.vpk>" installs before the GUI starts, then auto-boots the app.
    # Vita3K buffers its log and keeps the zip mtimes, so completion is detected from
    # the app folder (removed first; a reinstall replaces it anyway) plus the first
    # window, which only exists once the install returned.
    $appDir = Split-Path $eboot
    if (Test-Path $appDir) { Remove-Item $appDir -Recurse -Force }
    $v = Start-Vita3K "`"$vpk`"" $info.TitleId
    try {
        Wait-Until {
            $f = Get-Item $eboot -ErrorAction SilentlyContinue
            ($f -and $f.Length -eq $info.EbootSize -and @(Get-Vita3KWindows $v.Pid | Where-Object Visible).Count -gt 0)
        } 120 'archive install' $v.Pid | Out-Null
        Write-Step "installed $($info.TitleId) -> $(Split-Path $eboot)"
    } finally {
        Stop-Vita3K
        [Vita3KHarness.Native]::Close($v.Desk)
    }
    return $info.TitleId
}

function Invoke-Run([string]$titleId) {
    if (-not $titleId) { throw 'run: TITLE ID required' }
    if (-not (Test-Path (Join-Path $Portable "fs/ux0/app/$titleId/eboot.bin"))) { throw "run: $titleId is not installed" }
    $v = Start-Vita3K "-B $Renderer -r $titleId" $titleId
    try {
        $w = Wait-Until { Get-GameWindow $v.Pid $titleId } $BootTimeout "game window of $titleId" $v.Pid
        Write-Step "game window: '$($w.Title)' $($w.W)x$($w.H)"
        Start-Sleep -Seconds $Seconds
        if (-not (Get-Process -Id $v.Pid -ErrorAction SilentlyContinue)) { throw 'Vita3K exited during warm-up' }
    } catch {
        Stop-Vita3K; throw
    } finally {
        # The running process keeps the desktop alive; drop our handle.
        [Vita3KHarness.Native]::Close($v.Desk)
    }
}

function Invoke-Shot([string]$out) {
    if (-not $out) { throw 'shot: output path required' }
    $s = Get-State
    if (-not $s) { throw 'shot: Vita3K is not running' }
    $out = [IO.Path]::GetFullPath($out)
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    $w = Get-GameWindow $s.Pid $s.TitleId
    if (-not $w) { throw "shot: no game window for $($s.TitleId)" }

    # 1) Vita3K native screenshot (guest frame read back from the renderer).
    #    -PrintWindow skips it and goes straight to the window capture.
    $shotDir = Join-Path $Portable 'screenshots'
    $since = Get-Date
    $down = 1 -bor ($ShotScan -shl 16)
    $up = $down -bor (1 -shl 30) -bor (1L -shl 31)
    # WM_KEYDOWN then WM_KEYUP; Vita3K maps keys by the scan code in lParam bits 16-23.
    $png = $null
    if (-not $PrintWindow) { try {
        $err = [Vita3KHarness.Native]::Post($DesktopName, $w.Hwnd, [long[]]@(0x0100, $ShotVk, $down, 0x0101, $ShotVk, $up), 100)
        if ($err) { throw "posting the screenshot key failed: $err" }
        $png = Wait-Until {
            if (-not (Test-Path $shotDir)) { return $null }
            $f = Get-ChildItem $shotDir -Recurse -Filter *.png | Where-Object { $_.LastWriteTime -ge $since.AddSeconds(-1) } |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($f -and $f.Length -gt 0) { Start-Sleep -Milliseconds 300; return $f.FullName }
            $null
        } 10 'native screenshot' $s.Pid
    } catch { Write-Step "native screenshot failed: $($_.Exception.Message)" } }

    if ($png) {
        Copy-Item $png $out -Force
        Remove-Item $png -Force
        $how = 'vita3k-native'
    } elseif (-not $PrintWindow) {
        throw 'shot: native screenshot failed (see above); -PrintWindow is the fallback but it returns a blank image on the hidden desktop'
    } else {
        # 2) Opt-in: PrintWindow of the game window client area from a thread on the hidden
        #    desktop. Measured on this machine it gives a blank white frame for both Vulkan
        #    and OpenGL (no DWM redirection surface for a desktop that is never shown).
        $err = [Vita3KHarness.Native]::Capture($DesktopName, $w.Hwnd, $out)
        if ($err) { throw "shot: PrintWindow fallback failed: $err" }
        $how = 'printwindow'
    }
    $size = Get-PngSize $out
    Write-Step "screenshot ($how) $($size.Width)x$($size.Height): $out"
    if ($size.Width -ne 960 -or $size.Height -ne 544) { Write-Warning "expected 960x544, got $($size.Width)x$($size.Height)" }
    return $out
}

function Stop-Vita3K {
    $s = Get-State
    if (-not $s) { Write-Step 'not running'; return }
    $p = Get-Process -Id $s.Pid -ErrorAction SilentlyContinue
    # Close the main window only (the game window title carries "(TITLEID)"); with
    # confirmExitApp=false Vita3K shuts the running app down and exits.
    foreach ($w in (Get-Vita3KWindows $s.Pid | Where-Object { $_.Visible -and $_.Title -like 'Vita3K*' -and $_.Title -notmatch '\([A-Z0-9]{9}\)' })) {
        $err = [Vita3KHarness.Native]::Post($DesktopName, $w.Hwnd, [long[]]@(0x0010, 0, 0), 0)  # WM_CLOSE
        if ($err) { Write-Step "WM_CLOSE failed: $err" }
    }
    if ($p -and -not $p.WaitForExit(15000)) {
        Write-Step "pid $($s.Pid) did not exit after WM_CLOSE; killing"
        $p.Kill(); $p.WaitForExit(5000) | Out-Null
    }
    Remove-Item $StateFile -Force -ErrorAction SilentlyContinue
    Write-Step "stopped pid $($s.Pid)"
}

function Show-Status {
    $d = [Vita3KHarness.Native]::TryOpenDesktop($DesktopName)
    Write-Step "hidden desktop '$DesktopName': $(if ($d -ne [IntPtr]::Zero) { 'exists' } else { 'gone' })"
    [Vita3KHarness.Native]::Close($d)
    $s = Get-State
    if (-not $s) { Write-Step 'not running'; return }
    $s | Format-List
    Get-Vita3KWindows $s.Pid | Format-Table Hwnd, Visible, W, H, Class, Title -AutoSize
}

switch ($Command) {
    'setup'   { Invoke-Setup }
    'install' { Invoke-Install $Arg1 | Out-Null }
    'run'     { Invoke-Run $Arg1 }
    'shot'    { Invoke-Shot $Arg1 | Out-Null }
    'stop'    { Stop-Vita3K }
    'status'  { Show-Status }
    'smoke'   {
        if (-not $Arg2) { throw 'smoke: usage smoke <file.vpk> <out.png>' }
        $tid = Invoke-Install $Arg1
        try { Invoke-Run $tid; Invoke-Shot $Arg2 | Out-Null } finally { Stop-Vita3K }
    }
}
