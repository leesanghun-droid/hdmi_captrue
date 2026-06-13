Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct POINT { public int X; public int Y; }
public class Cur {
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
}
"@

$p = New-Object POINT
[Cur]::GetCursorPos([ref]$p) | Out-Null
Write-Output ("before: {0},{1}" -f $p.X, $p.Y)

$u = New-Object System.Net.Sockets.UdpClient
$u.Connect("192.168.0.8", 50000)

# Mouse absolute to center (X=Y=16384 of 0..32767)
$center = [byte[]](0x02, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00)
$u.Send($center, $center.Length) | Out-Null
Start-Sleep -Milliseconds 400
[Cur]::GetCursorPos([ref]$p) | Out-Null
Write-Output ("after center: {0},{1}" -f $p.X, $p.Y)

# Mouse absolute to top-left area (X=Y=3276 ~ 10%)
$tl = [byte[]](0x02, 0x00, 0xCC, 0x0C, 0xCC, 0x0C, 0x00)
$u.Send($tl, $tl.Length) | Out-Null
Start-Sleep -Milliseconds 400
[Cur]::GetCursorPos([ref]$p) | Out-Null
Write-Output ("after topleft: {0},{1}" -f $p.X, $p.Y)

$u.Close()
Write-Output "done"
