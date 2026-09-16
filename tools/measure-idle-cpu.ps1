# Measures how much CPU Light Host uses while tray-resident with no window open.
#
#   tools/measure-idle-cpu.ps1 [-Seconds 60]
#
# Why this exists: LookAndFeel.hpp promises the application costs nothing while
# idle, and the 5.1.0 metering deliberately keeps peak measurement always-on to
# make the clip indicator work when nobody is watching. That is a claim about
# cost, so it should be measured rather than asserted.
#
# TotalProcessorTime is used rather than a '% Processor Time' counter because the
# quantity of interest is tiny: a delta of CPU-seconds over a known wall-clock
# window gives a usable number at that scale, where an instantaneous percentage
# counter mostly reports zero and occasionally reports noise.
param(
    [int]$Seconds = 60
)

$proc = Get-Process -Name 'Light Host' -ErrorAction SilentlyContinue

if (-not $proc) {
    Write-Output 'Light Host is not running. Start it, close the Preferences window, and re-run.'
    exit 1
}

if ($proc.Count -gt 1) {
    Write-Output "More than one Light Host process is running; measuring pid $($proc[0].Id)."
    $proc = $proc[0]
}

$startCpu  = $proc.TotalProcessorTime
$startWall = Get-Date

Write-Output "pid $($proc.Id), sampling for $Seconds s. Leave the window closed."
Start-Sleep -Seconds $Seconds

$proc.Refresh()
$endCpu  = $proc.TotalProcessorTime
$endWall = Get-Date

$cpuSeconds  = ($endCpu - $startCpu).TotalSeconds
$wallSeconds = ($endWall - $startWall).TotalSeconds
$cores       = [Environment]::ProcessorCount

# Percentage of ONE core, which is the number that matters for a single audio
# thread, and percentage of the whole machine for context.
$ofOneCore = if ($wallSeconds -gt 0) { 100.0 * $cpuSeconds / $wallSeconds } else { 0 }

Write-Output ''
Write-Output ("wall clock     : {0:F1} s" -f $wallSeconds)
Write-Output ("CPU consumed   : {0:F3} s" -f $cpuSeconds)
Write-Output ("of one core    : {0:F3} %" -f $ofOneCore)
Write-Output ("of {0} cores    : {1:F4} %" -f $cores, ($ofOneCore / $cores))
Write-Output ''

Write-Output 'This is the WHOLE PROCESS, which is dominated by whatever plugins are in'
Write-Output 'the chain. A neural denoiser running inference on every block costs tens of'
Write-Output 'percent of a core on its own, so a number in that range says nothing about'
Write-Output 'metering either way.'
Write-Output ''
Write-Output 'To attribute the metering specifically, compare two runs that differ only in'
Write-Output 'the build -- the same chain, the same device, the same block size -- or run'
Write-Output 'an instance with an empty chain via -multi-instance=NAME. An absolute'
Write-Output 'threshold cannot separate them, and this script used to print a verdict as'
Write-Output 'though it could, which is how a measurement tool starts lying.'
