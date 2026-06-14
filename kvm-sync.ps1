# kvm-sync.ps1 - PC1 'share' folder as a real drive (default V:) with auto-PUSH.
#
# WHY ONLY AUTO-PUSH (and not auto-pull):
#   The board exposes ONE disk image to the target as a USB drive. Windows caches
#   that drive's file table (FAT). If we keep detaching/re-attaching it on a timer
#   while the target also writes to it, the target can flush a stale cached FAT and
#   wipe files. So we never poll. We only act when YOU drop a file into V:, which is
#   exactly the proven-safe manual 'push' - just automatic.
#
#   To bring target -> PC1 files back, run .\kvm-pull.ps1 on demand (when the target
#   is not actively writing to D:).
#
# Usage:
#   .\kvm-sync.ps1                 # drive V:, watch for drops
#   .\kvm-sync.ps1 -Drive K
#
param([string]$Drive = "V")
$ErrorActionPreference = "Stop"
$key   = "$env:USERPROFILE\.ssh\kvm"
$board = "root@192.168.0.8"
$share = Join-Path $PSScriptRoot "share"
New-Item -ItemType Directory -Force -Path $share | Out-Null

$dl = "${Drive}:"
if (-not (Test-Path $dl)) { & subst $dl $share; Start-Sleep -Milliseconds 300 }
if (Test-Path $dl) { Write-Host "drive $dl  ->  $share" -ForegroundColor Green }
else { Write-Warning "subst failed; using folder $share" }

$known    = @{}   # last value successfully pushed
$lastSeen = @{}   # value seen on the previous scan (for stability)
foreach ($f in Get-ChildItem -LiteralPath $share -File -EA SilentlyContinue) {
    $sig = "{0}|{1}" -f $f.Length, $f.LastWriteTimeUtc.Ticks
    $known[$f.Name] = $sig; $lastSeen[$f.Name] = $sig
}

function Push-Changed {
    $changed = @()
    foreach ($f in Get-ChildItem -LiteralPath $share -File -EA SilentlyContinue) {
        $sig = "{0}|{1}" -f $f.Length, $f.LastWriteTimeUtc.Ticks
        # only push once the file is stable (same as last scan) to avoid sending
        # a file that is still being copied into the drive
        if ($known[$f.Name] -ne $sig -and $lastSeen[$f.Name] -eq $sig) { $changed += $f }
        $lastSeen[$f.Name] = $sig
    }
    if ($changed.Count -eq 0) { return }
    Write-Host ("pushing {0} file(s) to target D: ..." -f $changed.Count) -ForegroundColor Cyan
    ssh -i $key -o BatchMode=yes $board "rm -rf /opt/kvm/inbox; mkdir -p /opt/kvm/inbox" 2>$null
    foreach ($f in $changed) {
        scp -i $key -o BatchMode=yes -- "$($f.FullName)" "${board}:/opt/kvm/inbox/$($f.Name)" 2>$null
    }
    ssh -i $key -o BatchMode=yes $board "/opt/kvm/kvm-share.sh sync" 2>$null | Out-Null
    foreach ($f in $changed) { $known[$f.Name] = "{0}|{1}" -f $f.Length, $f.LastWriteTimeUtc.Ticks }
    Write-Host "done; target re-reads D: in a few seconds." -ForegroundColor Green
}

Write-Host "watching $dl  -  drop files here to send to the target. Ctrl+C to stop." -ForegroundColor Cyan
Write-Host "(to receive files FROM the target, run .\kvm-pull.ps1)" -ForegroundColor DarkGray

# Simple, reliable local polling: scan the (small) share folder for new/changed
# files every ~1.5s. The board is only touched when something actually changes,
# so there is no periodic USB detach/attach when idle.
while ($true) {
    Start-Sleep -Milliseconds 1500
    try { Push-Changed } catch { Write-Warning "push error: $_" }
}
