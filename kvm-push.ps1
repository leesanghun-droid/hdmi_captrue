# Push files from PC1 to the target PC's USB drive (D:) via the board bridge.
# Usage: .\kvm-push.ps1 file1 [file2 ...]
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]]$Files)
$ErrorActionPreference = 'Stop'
$key   = "$env:USERPROFILE\.ssh\kvm"
$board = "root@192.168.0.8"

$remote = @()
foreach ($f in $Files) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Warning "not found: $f"; continue }
    $name = Split-Path -Leaf $f
    scp -i $key -o BatchMode=yes -- "$f" "${board}:/tmp/$name"
    $remote += "/tmp/$name"
}
if ($remote.Count -eq 0) { throw "no files to push" }
ssh -i $key -o BatchMode=yes $board "/opt/kvm/kvm-share.sh push $($remote -join ' ')"
Write-Host "OK: pushed $($remote.Count) item(s); target re-reads D: in a few seconds." -ForegroundColor Green
