Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinU {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$w = Get-Process msrdc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $w) { Write-Output "no msrdc window"; exit 1 }
[WinU]::ShowWindow($w[0].MainWindowHandle, 9) | Out-Null
[WinU]::SetForegroundWindow($w[0].MainWindowHandle) | Out-Null
Write-Output "fronted: $($w[0].MainWindowTitle)"
