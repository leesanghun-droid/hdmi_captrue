' Hidden launcher for the KVMShare agent: starts agent.ps1 with no visible
' window or console (STA so clipboard/SendKeys work).
Dim sh, here
Set sh = CreateObject("WScript.Shell")
here = Left(WScript.ScriptFullName, InStrRev(WScript.ScriptFullName, "\"))
sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File """ & here & "agent.ps1""", 0, False
