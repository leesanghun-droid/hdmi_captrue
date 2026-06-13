$ErrorActionPreference = 'SilentlyContinue'
Write-Output "=== Gadget USB devices (VID_1D6B) ==="
Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -match 'VID_1D6B' } |
    Select-Object Status, Class, FriendlyName, InstanceId |
    Format-Table -AutoSize -Wrap

Write-Output "=== HID devices ==="
Get-PnpDevice -PresentOnly -Class HIDClass |
    Where-Object { $_.InstanceId -match 'VID_1D6B' -or $_.FriendlyName -match 'HID' } |
    Select-Object Status, FriendlyName, InstanceId |
    Format-Table -AutoSize -Wrap

Write-Output "=== Disk drives (USB) ==="
Get-Disk | Where-Object { $_.BusType -eq 'USB' } |
    Select-Object Number, FriendlyName, @{N='SizeMB';E={[int]($_.Size/1MB)}}, PartitionStyle, OperationalStatus |
    Format-Table -AutoSize
