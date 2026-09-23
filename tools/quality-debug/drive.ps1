# Drives the shadPS4 window: focus, key presses with a hold time, and a window screenshot.
# Usage: . .\drive.ps1 ; Focus ; Key 'n' 150 ; Shot 'C:\path.png'
Add-Type @'
using System; using System.Runtime.InteropServices;
public class Drv {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -AssemblyName System.Drawing

$script:VK = @{ 'n'=0x4E; 'enter'=0x0D; 'up'=0x26; 'down'=0x28; 'left'=0x25; 'right'=0x27;
                'f12'=0x7B; 'b'=0x42; 'c'=0x43; 'v'=0x56; 'q'=0x51; 'u'=0x55; 'e'=0x45; 'o'=0x4F;
                'alt'=0x12; 'space'=0x20 }

function Win { (Get-Process shadps4 -ErrorAction Stop | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1).MainWindowHandle }

function Focus {
    $h = Win
    [Drv]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [Drv]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [void][Drv]::ShowWindow($h, 9); [void][Drv]::SetForegroundWindow($h); Start-Sleep -Milliseconds 300
}

# Extended keys (arrows) need KEYEVENTF_EXTENDEDKEY so SDL sees the right scancode.
function Key([string]$name, [int]$holdMs = 120) {
    $vk = [byte]$script:VK[$name]; $scan = [byte][Drv]::MapVirtualKey($vk, 0)
    $ext = if ($name -in 'up','down','left','right') { 1 } else { 0 }
    [Drv]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero); Start-Sleep -Milliseconds $holdMs
    [Drv]::keybd_event($vk, $scan, ($ext -bor 2), [UIntPtr]::Zero); Start-Sleep -Milliseconds 150
}

function KeyDown([string]$name) { $vk=[byte]$script:VK[$name]; [Drv]::keybd_event($vk, [byte][Drv]::MapVirtualKey($vk,0), 0, [UIntPtr]::Zero) }
function KeyUp([string]$name)   { $vk=[byte]$script:VK[$name]; [Drv]::keybd_event($vk, [byte][Drv]::MapVirtualKey($vk,0), 2, [UIntPtr]::Zero) }

# PrintWindow with PW_RENDERFULLCONTENT (2) captures the Vulkan swapchain on Windows 10+.
function Shot([string]$path, [double]$scale = 0.5) {
    $h = Win; $r = New-Object Drv+RECT; [void][Drv]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object Drawing.Bitmap $w, $ht; $g = [Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][Drv]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $out = New-Object Drawing.Bitmap ([int]($w*$scale)), ([int]($ht*$scale))
    $g2 = [Drawing.Graphics]::FromImage($out); $g2.DrawImage($bmp, 0, 0, $out.Width, $out.Height); $g2.Dispose()
    $out.Save($path, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); $out.Dispose()
}
