# kvm-drop.ps1 - send dropped files to the TARGET PC's Desktop.
# Files are staged into D:\_drop on the target's USB drive; the resident
# KVMShare agent on the target then silently copies them to the Desktop.
# No KVM typing, no Run dialog, no console - fully silent on the target side.
# Usage: .\kvm-drop.ps1 file1 [file2 ...]
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]]$Files)
$ErrorActionPreference = "Stop"
$key   = "$env:USERPROFILE\.ssh\kvm"
$board = "root@192.168.0.8"

# --- stage files on the board inbox ---------------------------------------
ssh -i $key -o BatchMode=yes $board "rm -rf /opt/kvm/inbox; mkdir -p /opt/kvm/inbox" 2>$null
$n = 0
foreach ($f in $Files) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Warning "not found: $f"; continue }
    $name = Split-Path -Leaf $f
    scp -i $key -o BatchMode=yes -- "$f" "${board}:/opt/kvm/inbox/$name" 2>$null
    $n++
}
if ($n -eq 0) { throw "no files to drop" }

# --- push into D:\_drop (agent on the target copies them to the Desktop) ---
$out = ssh -i $key -o BatchMode=yes $board "/opt/kvm/kvm-drop.sh" 2>&1
if ($LASTEXITCODE -ne 0 -or ($out -match 'ERROR')) {
    Write-Error ("drop failed: " + ($out -join '; '))
    exit 1
}
Write-Host "OK: dropped $n file(s) -> target Desktop" -ForegroundColor Green
