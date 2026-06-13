Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct POINT { public int X; public int Y; }
public class Cur { [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p); }
"@

$u = New-Object System.Net.Sockets.UdpClient
$u.Connect("192.168.0.8", 50000)

function MoveAbs([int]$x, [int]$y) {
    $pkt = [byte[]](0x02, 0x00, ($x -band 0xff), (($x -shr 8) -band 0xff), ($y -band 0xff), (($y -shr 8) -band 0xff), 0x00)
    $u.Send($pkt, $pkt.Length) | Out-Null
}

# Visible pattern: four corners -> center, then a smooth diagonal sweep.
$pts = @(
    @(1638,1638),    # ~5% top-left
    @(31129,1638),   # top-right
    @(31129,31129),  # bottom-right
    @(1638,31129),   # bottom-left
    @(16384,16384)   # center
)
foreach ($p in $pts) {
    MoveAbs $p[0] $p[1]
    Start-Sleep -Milliseconds 500
    $cp = New-Object POINT; [Cur]::GetCursorPos([ref]$cp) | Out-Null
    Write-Output ("sent ({0},{1})  -> cursor {2},{3}" -f $p[0], $p[1], $cp.X, $cp.Y)
}

# Smooth diagonal sweep
for ($i = 0; $i -le 20; $i++) {
    $v = [int](1638 + ($i / 20.0) * (31129 - 1638))
    MoveAbs $v $v
    Start-Sleep -Milliseconds 40
}
$u.Close()
Write-Output "done"
