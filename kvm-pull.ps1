# Pull the target PC's USB drive (D:) contents back to PC1 (into .\share).
# Usage: .\kvm-pull.ps1
$ErrorActionPreference = 'Stop'
$key   = "$env:USERPROFILE\.ssh\kvm"
$board = "root@192.168.0.8"
$dest  = Join-Path $PSScriptRoot 'share'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

ssh -i $key -o BatchMode=yes $board "/opt/kvm/kvm-share.sh pull"
# -r so it silently descends into dirs like 'System Volume Information'
scp -r -i $key -o BatchMode=yes "${board}:/opt/kvm/outbox/*" "$dest"
Write-Host "OK: pulled target D: contents into $dest" -ForegroundColor Green
Get-ChildItem -LiteralPath $dest | Select-Object Length, LastWriteTime, Name
