# Screenshot of the VM's desktop taken inside the guest (GDI CopyFromScreen of the composed
# desktop, DPI aware) - for D3D flip-model windows, which `prlctl capture` shows black.
# Used by screenshot.sh --guest. Writes the PNG to the path given as the first argument.
param([Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RnDpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
}
"@
[void][RnDpi]::SetProcessDpiAwarenessContext([IntPtr](-4))  # per-monitor v2: physical pixels
$x = [RnDpi]::GetSystemMetrics(76); $y = [RnDpi]::GetSystemMetrics(77)
$w = [RnDpi]::GetSystemMetrics(78); $h = [RnDpi]::GetSystemMetrics(79)
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"$Out ${w}x${h}"
