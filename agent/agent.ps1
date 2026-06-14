# KVMShare target agent (runs hidden, no console window).
#  FORWARD : copies new files from the KVMSHARE USB drive's _drop folder onto the
#            user's Desktop (silent).
#  REVERSE : when PC1 sends the grab hotkey (Ctrl+Alt+Shift+F9), copies the
#            currently selected/dragged file(s) onto the USB drive at _pull, so
#            PC1 can pull them. Triggered by the "drag a file off the screen"
#            gesture on PC1.
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public class HotKeyWin : NativeWindow {
    [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint mods, uint vk);
    public const int WM_HOTKEY = 0x0312;
    public Action OnHotKey;
    public HotKeyWin() { CreateHandle(new CreateParams()); }
    protected override void WndProc(ref Message m) {
        if (m.Msg == WM_HOTKEY && OnHotKey != null) OnHotKey();
        base.WndProc(ref m);
    }
}
"@ -ReferencedAssemblies System.Windows.Forms, System.Drawing

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WinFg {
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  const uint KEYUP = 0x0002;
  public static long Handle(){ return (long)GetForegroundWindow(); }
  public static string Title(){ var sb=new StringBuilder(300); GetWindowText(GetForegroundWindow(), sb, 300); return sb.ToString(); }
  // Force the hotkey modifiers up so a subsequent Ctrl+C is a clean copy.
  public static void ReleaseMods(){
    keybd_event(0x11,0,KEYUP,IntPtr.Zero);   // Ctrl
    keybd_event(0x12,0,KEYUP,IntPtr.Zero);   // Alt
    keybd_event(0x10,0,KEYUP,IntPtr.Zero);   // Shift
    keybd_event(0xA2,0,KEYUP,IntPtr.Zero); keybd_event(0xA3,0,KEYUP,IntPtr.Zero); // L/R Ctrl
    keybd_event(0xA4,0,KEYUP,IntPtr.Zero); keybd_event(0xA5,0,KEYUP,IntPtr.Zero); // L/R Alt
    keybd_event(0xA0,0,KEYUP,IntPtr.Zero); keybd_event(0xA1,0,KEYUP,IntPtr.Zero); // L/R Shift
  }
}
"@

function Get-ShareDrive {
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and $d.VolumeLabel -eq 'KVMSHARE') { return $d.Name.TrimEnd('\') }
    }
    return $null
}

# ---- FORWARD: _drop -> Desktop -------------------------------------------
$desktop = [Environment]::GetFolderPath('Desktop')
$seen = @{}
function Do-Forward {
    $drv = Get-ShareDrive
    if (-not $drv) { return }
    $drop = Join-Path $drv '_drop'
    if (-not (Test-Path $drop)) { return }
    foreach ($f in [System.IO.Directory]::GetFiles($drop)) {
        $name = [System.IO.Path]::GetFileName($f)
        if ($name -ieq 'go.bat') { continue }
        $fi  = New-Object System.IO.FileInfo $f
        $key = "$drv|$name|$($fi.Length)|$($fi.LastWriteTimeUtc.Ticks)"
        if ($seen.ContainsKey($key)) { continue }
        try { [System.IO.File]::Copy($f, (Join-Path $desktop $name), $true); $seen[$key] = 1 } catch {}
    }
}

# ---- REVERSE: grab hotkey -> copy selection to _pull ---------------------
# Read the file(s) currently selected in the foreground Explorer window via the
# Shell COM API (robust, focus-independent).
function Get-ExplorerSelection {
    $paths = @()
    try {
        $fg = [WinFg]::Handle()
        $sh = New-Object -ComObject Shell.Application
        foreach ($w in @($sh.Windows())) {
            try {
                if ([long]$w.HWND -eq $fg) {
                    foreach ($it in @($w.Document.SelectedItems())) {
                        if ($it.Path) { $paths += $it.Path }
                    }
                }
            } catch {}
        }
    } catch {}
    return $paths
}

function Do-Reverse {
    $drv = Get-ShareDrive
    if (-not $drv) { return }
    $pull = Join-Path $drv '_pull'
    if (-not (Test-Path $pull)) { New-Item -ItemType Directory -Path $pull -Force | Out-Null }

    # 1) Preferred: ask the foreground Explorer window what's selected.
    $paths = @(Get-ExplorerSelection)
    # 2) Fallback (desktop / non-Explorer): Ctrl+C and read the clipboard.
    if ($paths.Count -eq 0) {
        [WinFg]::ReleaseMods()                 # hotkey mods may still be down -> clean Ctrl+C
        Start-Sleep -Milliseconds 200
        [System.Windows.Forms.SendKeys]::SendWait('^c')
        Start-Sleep -Milliseconds 350
        $paths = @([System.Windows.Forms.Clipboard]::GetFileDropList())
    }
    if ($paths.Count -eq 0) { return }
    foreach ($src in $paths) {
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }   # files only
        $name = [System.IO.Path]::GetFileName($src)
        $tmp  = Join-Path $pull (".tmp_" + $name)
        $dst  = Join-Path $pull $name
        try {
            [System.IO.File]::Copy($src, $tmp, $true)
            [System.IO.File]::Move($tmp, $dst)   # atomic rename: board never pulls a partial file
        } catch {}
    }
}

$hk = New-Object HotKeyWin
# MOD_ALT=1 MOD_CONTROL=2 MOD_SHIFT=4 ; VK F9 = 0x78
[HotKeyWin]::RegisterHotKey($hk.Handle, 1, (1 -bor 2 -bor 4), 0x78) | Out-Null
$hk.OnHotKey = { Do-Reverse }

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1500
$timer.Add_Tick({ Do-Forward })
$timer.Start()

[System.Windows.Forms.Application]::Run()
