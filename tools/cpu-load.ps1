# Spins a chosen number of CPU-bound threads for a bounded time, so another
# measurement can be taken with the machine deliberately and repeatably busy.
#
#   tools/cpu-load.ps1
#   tools/cpu-load.ps1 -Threads 10 -Seconds 30
#   tools/cpu-load.ps1 -Threads 19 -Seconds 600 -MinimumFraction 0.4
#   tools/cpu-load.ps1 -Threads 19 -Seconds 600 -Quiet
#
# Why this exists: DECISIONS.md keeps Salvor over Alt Denoiser on latency and
# writes down the one thing that would reverse that -- "revisit if artifacts are
# ever audible under load". Load is therefore the independent variable of an
# experiment, and an independent variable has to be on a dial. "I opened a build
# and a Teams call" is not a load level anybody can reproduce next month. This
# puts a number on it: N threads, for N seconds, and a report of how much CPU
# was actually delivered.
#
# This machine has 20 logical cores (8 performance + 12 efficiency), so the
# default leaves one free -- pegging all 20 starves the thing being measured as
# well as the machine, and the interesting question is what a denoiser does
# while it still has somewhere to run.
#
# HOW THE THREADS ARE STOPPED, AND WHY THERE ARE TWO MECHANISMS
#
# A load generator that outlives its run poisons every measurement taken after
# it, silently, and the next person to notice is whoever cannot reproduce a
# figure. So stopping is belt and braces:
#
#   1. The finally block stops and disposes every runspace. This is what runs on
#      a normal finish and on Ctrl-C in an interactive session.
#   2. Every worker also carries its own absolute deadline, computed once before
#      any of them start. A worker that is somehow never told to stop still
#      stops, because it is watching the clock rather than waiting to be asked.
#
# Mechanism 2 is the one that matters when this is started as a background
# process by tools/denoiser-ab.ps1 and that process is killed: pass a -Seconds
# that comfortably exceeds the work it is covering and the load cannot outlive
# it even if the kill misses.
#
# THREADS ASKED FOR IS NOT CORES DELIVERED
#
# Measured here 2026-09-28, with nothing else deliberately running: 4 threads
# delivered 3.24 cores' worth of CPU time, 12 delivered 5.90, and 19 delivered
# 9.09 -- while the machine's own idle baseline sat between 24 % and 38 % of its
# 20 cores, from the software that is always on. Per-core counters during the
# 19-thread run showed all twenty cores at about 78 %, which is the same story
# read the other way: the load is real and spread, and the share this script
# gets is what the rest of the machine left.
#
# So the dial is "threads asked for" and the result is "cores delivered", and
# only the second one belongs on a results table. -MinimumFraction is the point
# below which the gap is called a failure rather than printed as a number
# nobody can act on; lower it deliberately on a box like this one, do not
# raise it to make a run go green.
#
# -Quiet prints nothing and reaches no verdict at all, because the only caller
# that uses it -- tools/denoiser-ab.ps1 -- kills this process as soon as its
# render finishes and reads the delivered figure off the process itself. A
# verdict printed into a pipe nobody reads, about a run that was cut short on
# purpose, would be worse than none.
param(
    [int]$Threads = ([Environment]::ProcessorCount - 1),
    [int]$Seconds = 30,
    [double]$MinimumFraction = 0.75,
    [switch]$Quiet
)

if ($Threads -lt 1) {
    Write-Output ("CPU LOAD TOOL FAIL: -Threads must be at least 1, got {0}" -f $Threads)
    exit 2
}

if ($Seconds -lt 1) {
    Write-Output ("CPU LOAD TOOL FAIL: -Seconds must be at least 1, got {0}" -f $Seconds)
    exit 2
}

if ($MinimumFraction -le 0.0 -or $MinimumFraction -gt 1.0) {
    Write-Output ("CPU LOAD TOOL FAIL: -MinimumFraction must be above 0 and at most 1, got {0}" -f $MinimumFraction)
    exit 2
}

$cores = [Environment]::ProcessorCount

if ($Threads -gt $cores * 2) {
    Write-Output ("CPU LOAD TOOL FAIL: -Threads {0} is more than twice the {1} logical cores on this machine; that measures the scheduler, not the load" -f $Threads, $cores)
    exit 2
}

# Integer and floating-point arithmetic in a loop, with nothing allocated inside
# it. Allocating would turn this into a test of the garbage collector, which is
# a different kind of busy from the one a denoiser competes with.
$spin = {
    param([datetime]$Deadline)

    $x = 1.0
    $n = 0

    while ([datetime]::UtcNow -lt $Deadline) {
        # The clock is read once per batch rather than once per iteration: a
        # DateTime read costs more than the arithmetic it would be guarding, so
        # checking every time would measure the clock instead of the CPU. The
        # batch is small enough that the thread still stops within milliseconds
        # of its deadline.
        for ($i = 0; $i -lt 50000; $i++) {
            $x = ($x * 1.0000001) + 0.5
            $n++
        }
    }

    return $n
}

$deadline  = [datetime]::UtcNow.AddSeconds($Seconds)
$self      = [System.Diagnostics.Process]::GetCurrentProcess()
$startCpu  = $self.TotalProcessorTime
$startWall = Get-Date

if (-not $Quiet) {
    Write-Output ("threads           : {0} of {1} logical cores" -f $Threads, $cores)
    Write-Output ("duration          : {0} s (each thread carries this as its own deadline)" -f $Seconds)
}

$pool = [runspacefactory]::CreateRunspacePool(1, $Threads)
$pool.Open()

$shells = New-Object System.Collections.Generic.List[object]

try {
    for ($t = 0; $t -lt $Threads; $t++) {
        $ps = [powershell]::Create()
        $ps.RunspacePool = $pool
        $null = $ps.AddScript($spin).AddArgument($deadline)
        $shells.Add([pscustomobject]@{ Shell = $ps; Handle = $ps.BeginInvoke() })
    }

    # Polled rather than WaitHandle::WaitAll, which caps at 64 handles and
    # behaves differently in an STA host. 200 ms of a sleeping main thread costs
    # nothing next to the workers.
    while ($shells | Where-Object { -not $_.Handle.IsCompleted }) {
        Start-Sleep -Milliseconds 200
    }
}
finally {
    foreach ($entry in $shells) {
        if (-not $entry.Handle.IsCompleted) {
            $entry.Shell.Stop()
        }

        $entry.Shell.Dispose()
    }

    $pool.Close()
    $pool.Dispose()
}

$self.Refresh()
$cpuSeconds  = ($self.TotalProcessorTime - $startCpu).TotalSeconds
$wallSeconds = ((Get-Date) - $startWall).TotalSeconds
$delivered   = if ($wallSeconds -gt 0) { $cpuSeconds / $wallSeconds } else { 0.0 }

if ($Quiet) {
    exit 0
}

Write-Output ''
Write-Output ("wall clock        : {0:F1} s" -f $wallSeconds)
Write-Output ("CPU consumed      : {0:F1} s" -f $cpuSeconds)
Write-Output ("cores equivalent  : {0:F2} of {1} requested, on a {2}-core machine" -f $delivered, $Threads, $cores)
Write-Output ("machine busied    : {0:F1} %" -f (100.0 * $delivered / $cores))
Write-Output ''

# The house habit: say what this figure is not. It is this process's own CPU
# time, which is exactly the load this script added and says nothing about what
# else was running. On a machine that was already busy the same threads deliver
# less, and that is worth knowing BEFORE a load level goes on a results table --
# it is the difference between "19 threads" and "19 threads' worth of CPU".
Write-Output 'This is this process''s own CPU time, so it measures the load ADDED, not the'
Write-Output 'load present. A cores-equivalent well below the thread count means the'
Write-Output 'scheduler could not give each thread a core -- something else was competing,'
Write-Output 'or the efficiency cores were doing the work. Re-run on an otherwise idle'
Write-Output 'machine before quoting the level.'

if ($delivered -lt $Threads * $MinimumFraction) {
    Write-Output ''
    Write-Output ("CPU LOAD FAIL: asked for {0} threads' worth and delivered {1:F2}, below the {2:P0} floor" -f $Threads, $delivered, $MinimumFraction)
    Write-Output 'Quote the delivered figure, not the requested one, or re-run with less'
    Write-Output 'competing for the machine.'
    exit 1
}

exit 0
