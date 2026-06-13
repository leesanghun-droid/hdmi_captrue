Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Kb { [DllImport("user32.dll")] public static extern short GetKeyState(int n); }
"@
function NumLockOn { return (([Kb]::GetKeyState(0x90)) -band 1) -eq 1 }

Write-Output ("NumLock before: {0}" -f (NumLockOn))
$u = New-Object System.Net.Sockets.UdpClient
$u.Connect("192.168.0.8", 50000)
# Keyboard: press NumLock (HID 0x53), then release
$down = [byte[]](0x01, 0x00, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00)
$up   = [byte[]](0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
$u.Send($down, $down.Length) | Out-Null
Start-Sleep -Milliseconds 60
$u.Send($up, $up.Length) | Out-Null
Start-Sleep -Milliseconds 300
$u.Close()
Write-Output ("NumLock after:  {0}" -f (NumLockOn))
