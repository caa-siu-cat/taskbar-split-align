param([string]$Name = 'taskbar', [int]$Height = 110)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CaptureDpiContext {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
}
'@
$previousDpi = [CaptureDpiContext]::SetThreadDpiAwarenessContext([IntPtr](-4))
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$image = [System.Drawing.Bitmap]::new($bounds.Width, $Height)
$graphics = [System.Drawing.Graphics]::FromImage($image)
try {
    $graphics.CopyFromScreen($bounds.Left, $bounds.Bottom-$Height, 0, 0, $image.Size)
    $destination = Join-Path $PSScriptRoot "verification\$Name.png"
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    $image.Save($destination, [System.Drawing.Imaging.ImageFormat]::Png)
    $destination
} finally {
    $graphics.Dispose(); $image.Dispose()
    [CaptureDpiContext]::SetThreadDpiAwarenessContext($previousDpi) | Out-Null
}
