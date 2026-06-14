# kvm-grab.ps1 - fetch the file the target agent just staged (reverse drag-out).
# Triggered by DeckLinkPreview when you drag a file off the target screen.
# The target agent has copied the dragged file(s) to D:\_pull; the board moves
# them to revout; we copy them onto PC1's Desktop.
$ErrorActionPreference = 'Stop'
$key     = "$env:USERPROFILE\.ssh\kvm"
$board   = "root@192.168.0.8"
$desktop = [Environment]::GetFolderPath('Desktop')

# give the target agent a moment to finish writing _pull
Start-Sleep -Milliseconds 1500

$out = ssh -i $key -o BatchMode=yes $board "/opt/kvm/kvm-share.sh reverse" 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error ("reverse failed: " + ($out -join '; ')); exit 1 }

$list = ssh -i $key -o BatchMode=yes $board "cd /opt/kvm/revout 2>/dev/null && find . -maxdepth 1 -type f -printf '%f\n'" 2>$null
$n = 0
foreach ($name in $list) {
    if (-not $name) { continue }
    scp -i $key -o BatchMode=yes -- "${board}:/opt/kvm/revout/$name" "$desktop\$name" 2>$null
    if ($LASTEXITCODE -eq 0) { $n++ }
}
ssh -i $key -o BatchMode=yes $board "rm -f /opt/kvm/revout/*" 2>$null | Out-Null

if ($n -eq 0) { Write-Error "nothing was grabbed (no file selected on the target?)"; exit 2 }
Write-Host "OK: grabbed $n file(s) from target -> $desktop" -ForegroundColor Green
