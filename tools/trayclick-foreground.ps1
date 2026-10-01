<#
.SYNOPSIS
  Measures whether a left-click on the tray icon puts Preferences in the
  FOREGROUND, or leaves it behind the window the user was working in.

.DESCRIPTION
  This is the check behind the 5.6.2 fix, kept because it can fail. It launches
  the given binary as its own instance (-multi-instance, so the real settings
  file is never touched), finds the notification icon through UI Automation --
  including the Windows 11 overflow flyout, where a freshly built exe always
  lands -- clicks it with injected mouse input, and reports which process owns
  the foreground window afterwards.

  Injected input, not the icon's Invoke pattern, because the event under test
  is the shell's foreground grant around a real button-up, and Invoke does not
  go through it.

  Run it against the previous release first. If that passes, the harness is
  not measuring anything: on 2026-09-30 the shipped 5.6.1 reported
  "PREFERENCES IS NOT FOREGROUND" and the 5.6.2 build reported the opposite,
  with the same procedure and Visual Studio Code holding the foreground.

  Two things about the Windows 11 tray that cost an hour: the icon's
  automation Name carries a LEADING SPACE (" Light Host"), and hidden icons
  live in a separate top-level window of class
  TopLevelWindowForOverflowXamlIsland that exists only while the flyout is
  open.

.PARAMETER Exe
  The "Light Host.exe" to test.
.PARAMETER Instance
  Suffix for -multi-instance. Its settings and state are deleted afterwards.
.PARAMETER SettleMs
  How long to wait after the click before reading the foreground window.

.EXAMPLE
  tools\trayclick-foreground.ps1 -Exe "build\release\LightHost_artefacts\Release\Light Host.exe" -Instance fgcheck

  Exit 0: Preferences is the foreground window.
  Exit 1: it is not (the 5.6.1 behaviour).
  Exit 2: setup failed before any click happened (no icon found, usually
          because the application did not start).
#>
param(
  [Parameter(Mandatory)][string]$Exe,
  [string]$Instance = 'fgcheck',
  [int]$SettleMs = 2500
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FgNative {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
  public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
  public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
}
'@
Add-Type -TypeDefinition $sig

$A = [System.Windows.Automation.AutomationElement]
$T = [System.Windows.Automation.TreeScope]
$btnCond = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)

function Say([string]$s) { [Console]::Out.WriteLine($s) }

function Get-Taskbar {
  $A::RootElement.FindFirst($T::Children, (New-Object System.Windows.Automation.PropertyCondition($A::ClassNameProperty, 'Shell_TrayWnd')))
}

function Find-TrayButton([string]$namePrefix) {
  $tray = Get-Taskbar
  if ($tray) {
    foreach ($b in $tray.FindAll($T::Descendants, $btnCond)) {
      if ($b.Current.Name.Trim() -like "$namePrefix*" -and $b.Current.AutomationId -eq 'NotifyItemIcon') { return @{ el = $b; where = 'taskbar' } }
    }
  }
  foreach ($w in $A::RootElement.FindAll($T::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
    $cls = $w.Current.ClassName
    if ($cls -like '*Overflow*' -or $cls -eq 'NotifyIconOverflowWindow') {
      foreach ($b in $w.FindAll($T::Descendants, $btnCond)) {
        if ($b.Current.Name.Trim() -like "$namePrefix*") { return @{ el = $b; where = "overflow:$cls" } }
      }
    }
  }
  return $null
}

function Open-Overflow {
  $tray = Get-Taskbar
  if (-not $tray) { return $false }
  foreach ($b in $tray.FindAll($T::Descendants, $btnCond)) {
    $n = $b.Current.Name
    if ($n -like 'Show Hidden Icons*' -or $n -like '*hidden icons*' -or $b.Current.AutomationId -eq 'NotificationChevronButton') {
      # The flyout is opened through its own Invoke pattern; only the click on
      # the icon itself has to be real input.
      try { ($b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke() }
      catch { Click-Element $b }
      Start-Sleep -Milliseconds 1000
      return $true
    }
  }
  return $false
}

function Click-Element($el) {
  $r = $el.Current.BoundingRectangle
  $x = [int]($r.X + $r.Width / 2); $y = [int]($r.Y + $r.Height / 2)
  [void][FgNative]::SetCursorPos($x, $y)
  Start-Sleep -Milliseconds 120
  [FgNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down
  Start-Sleep -Milliseconds 90
  [FgNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # left up
  Say "clicked at $x,$y (element '$($el.Current.Name)')"
}

function Report-Foreground([uint32]$ourPid, [string]$label) {
  $h = [FgNative]::GetForegroundWindow()
  $fpid = [FgNative]::Pid($h)
  $title = [FgNative]::Title($h)
  $proc = try { (Get-Process -Id $fpid -ErrorAction Stop).ProcessName } catch { '?' }
  Say ("{0}: foreground pid={1} ({2}) title='{3}' ours={4}" -f $label, $fpid, $proc, $title, ($fpid -eq $ourPid))
  return ($fpid -eq $ourPid)
}

function List-OurWindows([uint32]$ourPid) {
  $found = New-Object System.Collections.Generic.List[string]
  $cb = [FgNative+EnumProc]{ param($h, $l)
    if ([FgNative]::Pid($h) -eq $ourPid -and [FgNative]::IsWindowVisible($h)) {
      $t = [FgNative]::Title($h); if ($t) { $found.Add($t) }
    }
    return $true }
  [void][FgNative]::EnumWindows($cb, [IntPtr]::Zero)
  Say ("our visible top-level windows: " + ($found -join ' | '))
}

# ---- run -------------------------------------------------------------------
$origCursor = New-Object FgNative+POINT; [void][FgNative]::GetCursorPos([ref]$origCursor)
Say "exe: $Exe"
Say ("exe FileVersion: " + (Get-Item -LiteralPath $Exe).VersionInfo.FileVersion)
$p = Start-Process -FilePath $Exe -ArgumentList "-multi-instance=$Instance" -PassThru
Say "launched pid $($p.Id)"
Start-Sleep -Milliseconds 3500
[void](Report-Foreground $p.Id 'before click')

$hit = $null
for ($i = 0; $i -lt 6 -and -not $hit; $i++) {
  $hit = Find-TrayButton 'Light Host'
  if (-not $hit) {
    if (-not (Open-Overflow)) { Say 'no overflow chevron found' }
    $hit = Find-TrayButton 'Light Host'
  }
  if (-not $hit) { Start-Sleep -Milliseconds 800 }
}
$logDir = Join-Path $env:APPDATA 'Light Host'
if (-not $hit) {
  Say 'TRAY ICON NOT FOUND'
  List-OurWindows $p.Id
  Stop-Process -Id $p.Id -Force
  exit 2
}
Say "tray icon found in $($hit.where)"
Click-Element $hit.el
Start-Sleep -Milliseconds $SettleMs

$ok = Report-Foreground $p.Id 'after click'
List-OurWindows $p.Id
[void][FgNative]::SetCursorPos($origCursor.X, $origCursor.Y)

# The application's own verdict line, where the build writes one (5.6.2+).
Get-ChildItem $logDir -Filter '*.log' | Where-Object LastWriteTime -gt (Get-Date).AddMinutes(-2) | ForEach-Object {
  $lines = Select-String -LiteralPath $_.FullName -Pattern 'raised:|opening Preferences' | Select-Object -Last 2
  foreach ($l in $lines) { Say ("log [{0}]: {1}" -f $_.Name, $l.Line.Trim()) }
}

Stop-Process -Id $p.Id -Force
Start-Sleep -Milliseconds 500
Get-ChildItem $logDir -Filter "Light Host.$Instance.*" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
if ($ok) { Say 'RESULT: PREFERENCES IS FOREGROUND'; exit 0 } else { Say 'RESULT: PREFERENCES IS NOT FOREGROUND'; exit 1 }
