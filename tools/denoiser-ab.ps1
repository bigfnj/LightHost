# Renders one take through each denoiser candidate at several CPU load levels,
# repeats every combination, and reports medians and spread instead of a single
# run nobody can reproduce.
#
#   tools/denoiser-ab.ps1
#   tools/denoiser-ab.ps1 -Candidates Salvor -LoadLevels 0,19 -Repeats 3
#   tools/denoiser-ab.ps1 -Take tools/voice_test.wav -Repeats 7 -KeepRenders
#   tools/denoiser-ab.ps1 -Take long-take.wav -Sections 'typing:12-24','speech:30-60'
#
# Why this exists: DECISIONS.md keeps Salvor over Alt Denoiser on latency -- 10 ms
# against 40 ms for the same DeepFilterNet3 model -- and writes down the single
# condition that would reverse that choice: "revisit if artifacts are ever
# audible under load. Alt Denoiser is provably immune, being synchronous."
# That sentence has been a guess since the day it was written, because nobody
# has put the machine under a known load and measured what happens. This turns
# it into a measurement that either fires the trigger or clears it.
#
# WHY THE REGRESSION GATE'S METHOD CANNOT BE REUSED
#
# tools/render-regression.sh decides by sha256 equality, and that model does not
# reach here. Salvor runs its inference on a worker thread and its output is
# NOT repeatable run to run -- DECISIONS.md records four paced renders of one
# input producing two different hashes with the render keeping pace throughout.
# A hash comparison on this chain fails at random. So every combination is
# repeated and judged on the median and the spread of the repeats, and the
# spread is reported rather than hidden: it is the thing that decides whether
# any difference between two candidates is real.
#
# WHAT THE SCORE IS MEASURED AGAINST
#
# Each candidate is scored against a DRY reference of the same take, so the
# figure is noise reduction achieved rather than an absolute level that depends
# on how loud the take was. By default the reference is the take itself, which
# is exact: with no plugin in it the host's path is the input node, a lane gain
# left at 0 dB, and the output node. There is no way to ask the CLI for an
# empty-chain render -- --chain "" falls back to the saved live chain, which on
# this machine contains a denoiser -- so a rendered reference would have to go
# through SOME plugin and would fold that plugin's behaviour into the baseline.
# -DryChain renders a control chain instead when that is what you want.
#
# WHAT THE EXIT CODE MEANS
#
#   0  every candidate held up: none lost more than -AudibleDb of noise
#      reduction between the lightest and heaviest load, and the spread was
#      tight enough for that to mean something. The reversal trigger stays
#      clear.
#   1  a candidate did lose that much. DECISIONS.md's reversal trigger for the
#      denoiser choice is met and the decision needs revisiting.
#   2  no verdict was reached. Bad arguments, no build, no take -- or the
#      repeats disagreed with each other by more than the difference being
#      looked for, which is a measurement that cannot justify a verdict and
#      must not print one. Raise -Repeats and run it again.
param(
    [string[]]$Candidates    = @('Salvor', 'Alt Denoiser'),
    [int[]]$LoadLevels       = @(0, 10, 19),
    [int]$Repeats            = 5,
    [string]$Take            = '',
    [string[]]$Sections      = @(),
    [string]$DryChain        = '',
    [double]$AudibleDb       = 3.0,
    [int]$TimeoutSeconds     = 0,
    [string]$Python          = '',
    [string]$OutputDir       = '',
    [switch]$KeepRenders
)

$ErrorActionPreference = 'Stop'

# PowerShell 7.4 turned a native command's non-zero exit code into a terminating
# error when ErrorActionPreference is Stop. Everything here that shells out --
# the render, the scorer -- reports through its exit code on purpose and reads
# it deliberately, so that behaviour would turn a handled result into a crash
# and lose the message the tool printed. Assigning this on 5.1 is harmless.
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot   = Split-Path -Parent $PSScriptRoot
$loadScript = Join-Path $PSScriptRoot 'cpu-load.ps1'
$scorer     = Join-Path $PSScriptRoot 'score-sections.py'

# Where the build can be, in the order it is looked for -- the same list
# tools/render-regression.sh uses, for the same reason: JUCE puts products under
# <target>_artefacts/<CONFIG>/ for the single-config Ninja presets as well as
# the multi-config MSVC one, so what differs between platforms is the build
# directory and the file at the end of it.
$exeCandidates = @(
    "$repoRoot/build/release/LightHost_artefacts/Release/Light Host.exe"
    "$repoRoot/build/ninja-release/LightHost_artefacts/Release/Light Host.exe"
    "$repoRoot/build/ninja-release/LightHost_artefacts/Release/Light Host.app/Contents/MacOS/Light Host"
    "$repoRoot/build/ninja-release/LightHost_artefacts/Release/Light Host"
)

#region helpers

function Get-Median
{
    param([double[]]$Values)

    if ($Values.Count -eq 0) { return [double]::NaN }

    $sorted = $Values | Sort-Object
    $middle = [int][Math]::Floor($sorted.Count / 2)

    if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$middle] }

    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Read-WavFacts
{
    <#
        Sample rate and duration from the header, without reading the audio.
        Used to print an honest time estimate before a sweep that can run for
        the best part of an hour, and to refuse a file that is not PCM WAV
        before spending any of it.
    #>
    param([string]$Path)

    $head = New-Object byte[] 4096
    $stream = [System.IO.File]::OpenRead($Path)

    try   { $read = $stream.Read($head, 0, $head.Length) }
    finally { $stream.Dispose() }

    if ($read -lt 44 -or
        [Text.Encoding]::ASCII.GetString($head, 0, 4) -ne 'RIFF' -or
        [Text.Encoding]::ASCII.GetString($head, 8, 4) -ne 'WAVE')
    {
        return $null
    }

    $rate = 0; $channels = 0; $bits = 0; $byteRate = 0; $dataBytes = 0
    $pos = 12

    while ($pos + 8 -le $read)
    {
        $id   = [Text.Encoding]::ASCII.GetString($head, $pos, 4)
        $size = [int][BitConverter]::ToUInt32($head, $pos + 4)

        if ($id -eq 'fmt ')
        {
            $channels = [int][BitConverter]::ToUInt16($head, $pos + 10)
            $rate     = [int][BitConverter]::ToUInt32($head, $pos + 12)
            $byteRate = [int][BitConverter]::ToUInt32($head, $pos + 16)
            $bits     = [int][BitConverter]::ToUInt16($head, $pos + 22)
        }
        elseif ($id -eq 'data')
        {
            $dataBytes = $size
            break
        }

        if ($size -le 0) { break }

        $pos += 8 + $size + ($size % 2)
    }

    if ($byteRate -le 0) { return $null }

    # A data size of zero or one larger than the file means the header was
    # written by something streaming; the file on disk is the better answer.
    $onDisk = (New-Object System.IO.FileInfo $Path).Length - 44

    if ($dataBytes -le 0 -or $dataBytes -gt $onDisk) { $dataBytes = $onDisk }

    return [pscustomobject]@{
        SampleRate = $rate
        Channels   = $channels
        Bits       = $bits
        Seconds    = $dataBytes / [double]$byteRate
    }
}

function Get-RenderFacts
{
    <#
        Everything the render prints that this sweep judges a run by.

        blocksTotal deserves a note. HostStartup.cpp prints it ONLY inside the
        "render fell behind" NOTE, which it only prints when blocksBehind is
        greater than zero. So a healthy run reports no block counts at all, and
        the honest reading of that is "0 behind, total not stated" -- which is
        why BlocksTotalKnown exists and why the table prints a dash rather than
        a number it would have had to invent.
    #>
    param([string[]]$Lines)

    $facts = [pscustomobject]@{
        Ok               = $false
        Message          = ''
        Pacing           = ''
        PluginsActive    = 0
        PluginsFailed    = 0
        Connections      = 0
        LatencySamples   = 0
        LatencyMs        = 0.0
        FramesWritten    = 0L
        RealtimeFactor   = [double]::NaN
        KeptPace         = $false
        BlocksBehind     = 0
        BlocksTotal      = 0
        BlocksTotalKnown = $false
        Warnings         = @()
    }

    foreach ($line in $Lines)
    {
        if ($line -match '^RENDER OK')                                   { $facts.Ok = $true }
        elseif ($line -match '^RENDER FAIL:\s*(.*)$')                    { $facts.Message = $Matches[1] }
        elseif ($line -match '^\s*pacing\s*:\s*(.+?)\s*$')               { $facts.Pacing = $Matches[1] }
        elseif ($line -match '^\s*plugins active\s*:\s*(\d+)')           { $facts.PluginsActive = [int]$Matches[1] }
        elseif ($line -match '^\s*plugins failed\s*:\s*(\d+)')           { $facts.PluginsFailed = [int]$Matches[1] }
        elseif ($line -match '^\s*connections\s*:\s*(\d+)')              { $facts.Connections = [int]$Matches[1] }
        elseif ($line -match '^\s*frames written\s*:\s*(\d+)')           { $facts.FramesWritten = [long]$Matches[1] }
        elseif ($line -match '^\s*declared latency\s*:\s*(\d+) samples \(([0-9.]+) ms\)')
        {
            $facts.LatencySamples = [int]$Matches[1]
            $facts.LatencyMs      = [double]$Matches[2]
        }
        elseif ($line -match '^\s*realtime factor\s*:\s*([0-9.]+)')
        {
            $facts.RealtimeFactor = [double]$Matches[1]
            $facts.KeptPace       = $line -match '\(kept pace\)'
        }
        elseif ($line -match '^\s*NOTE\s*:\s*render fell behind real time on (\d+) of (\d+) blocks')
        {
            $facts.BlocksBehind     = [int]$Matches[1]
            $facts.BlocksTotal      = [int]$Matches[2]
            $facts.BlocksTotalKnown = $true
        }
        elseif ($line -match '^\s*WARNING\s*:\s*(.*)$')
        {
            $facts.Warnings += $Matches[1]
        }
    }

    return $facts
}

function Start-CpuLoad
{
    <#
        The load generator, as a separate process so it can be killed outright.

        -Seconds is a ceiling rather than the intended duration: the caller
        stops it as soon as the render finishes. It is passed anyway because
        cpu-load.ps1 gives every worker its own deadline, so a kill that somehow
        misses still cannot leave the machine loaded for the next run.
    #>
    param([string]$Script, [int]$Threads, [int]$Seconds)

    if ($Threads -le 0) { return $null }

    # This host's own executable, so a sweep run under powershell.exe does not
    # silently need pwsh installed, and one run under pwsh gets pwsh.
    $shell = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName

    return Start-Process -FilePath $shell -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile'
        '-ExecutionPolicy'; 'Bypass'
        '-File'; ('"{0}"' -f $Script)
        '-Threads'; $Threads
        '-Seconds'; $Seconds
        '-Quiet'
    )
}

function Stop-CpuLoad
{
    <#
        Kills the load and reports what it actually delivered, in cores.

        The delivered figure is read before the kill and reported per run,
        because the requested thread count is a dial setting and not a result:
        on a machine that is already busy, N threads deliver well under N cores
        and the table would otherwise be labelled with a load that never
        happened.
    #>
    param($Process, [datetime]$StartedAt)

    if ($null -eq $Process) { return 0.0 }

    $cores = 0.0

    try
    {
        if (-not $Process.HasExited)
        {
            $Process.Refresh()
            $wall = ((Get-Date) - $StartedAt).TotalSeconds

            if ($wall -gt 0) { $cores = $Process.TotalProcessorTime.TotalSeconds / $wall }
        }
    }
    catch { $cores = 0.0 }

    try
    {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $null = $Process.WaitForExit(5000)
    }
    catch { }

    return $cores
}

function Invoke-Render
{
    <#
        One paced render, captured. Paced is not optional here: an unpaced
        render starves a worker-thread plugin, which then passes the dry signal
        through and reports success -- the exact failure DECISIONS.md records
        costing an afternoon and producing a confidently wrong verdict.

        Start-Process with redirection rather than the call operator, so a
        plugin that wedges is killed on a timeout instead of hanging the sweep
        with the CPU load still running behind it.
    #>
    param(
        [string]$Exe,
        [string]$InputFile,
        [string]$OutputFile,
        [string]$Chain,
        [int]$TimeoutSeconds,
        [string]$ScratchDir
    )

    $arguments = @(
        '--render'
        ('"{0}"' -f $InputFile)
        ('"{0}"' -f $OutputFile)
        '--chain'
        ('"{0}"' -f $Chain)
    )

    $started = Get-Date
    $process = $null
    $outLog = $null
    $errLog = $null

    # Retried with a backoff, because this launch intermittently fails with
    # "the process cannot access the file because it is being used by another
    # process" -- three times while this script was being written, once for
    # three attempts in a row across three seconds, and never reproducibly.
    # Start-Process reports the same message whether the sharing violation was
    # on the executable or on a redirect target, so which file it was is not
    # knowable from here; both the build tree and %TEMP% on this machine are
    # watched by something that opens files behind you, a OneDrive-synced
    # working copy being the obvious suspect. Retrying is not a fix and is not
    # pretending to be one -- it is refusing to lose a fifty-minute sweep to a
    # three-second lock on a scratch file. Fresh log names each attempt, so a
    # genuinely stuck one is abandoned rather than waited on.
    $attempts = 5

    foreach ($attempt in 1..$attempts)
    {
        $stamp  = [guid]::NewGuid().ToString('N').Substring(0, 8)
        $outLog = Join-Path $ScratchDir "render-$stamp.out"
        $errLog = Join-Path $ScratchDir "render-$stamp.err"

        try
        {
            $process = Start-Process -FilePath $Exe -ArgumentList $arguments -NoNewWindow -PassThru `
                                     -RedirectStandardOutput $outLog -RedirectStandardError $errLog
            break
        }
        catch
        {
            if ($attempt -eq $attempts) { throw }

            Start-Sleep -Seconds $attempt
        }
    }

    $finished = $process.WaitForExit($TimeoutSeconds * 1000)

    if (-not $finished)
    {
        try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch { }
        $null = $process.WaitForExit(5000)
    }

    # [System.IO.File] rather than Get-Content and Remove-Item, and the same
    # everywhere this script touches its scratch directory. Remove-Item's
    # -LiteralPath mis-handles a path containing a tilde -- it reports "an
    # object at the specified path C:\Users\JUSTIN~1.LOW does not exist",
    # naming a prefix of the path rather than the path -- and an 8.3 short
    # name is exactly what Windows generates for a profile directory with a
    # dot in it, so %TEMP% really can look like that. -ErrorAction
    # SilentlyContinue does not suppress it, and it took down a sweep.
    $lines = @()
    if ([System.IO.File]::Exists($outLog)) { $lines += @([System.IO.File]::ReadAllLines($outLog)) }
    if ([System.IO.File]::Exists($errLog)) { $lines += @([System.IO.File]::ReadAllLines($errLog)) }

    foreach ($log in @($outLog, $errLog))
    {
        try { [System.IO.File]::Delete($log) } catch { }
    }

    $facts = Get-RenderFacts -Lines $lines
    Add-Member -InputObject $facts -NotePropertyName 'WallSeconds' -NotePropertyValue ((Get-Date) - $started).TotalSeconds
    Add-Member -InputObject $facts -NotePropertyName 'TimedOut' -NotePropertyValue (-not $finished)
    Add-Member -InputObject $facts -NotePropertyName 'ExitCode' -NotePropertyValue $(if ($finished) { $process.ExitCode } else { -1 })

    if ($facts.TimedOut) { $facts.Message = "no result after $TimeoutSeconds s; the render was killed" }

    # A render that neither said RENDER OK nor said why still has to carry an
    # explanation into the table, or the failure list is a column of blanks.
    if (-not $facts.Ok -and -not $facts.Message)
    {
        $facts.Message = "exit code $($facts.ExitCode) and no RENDER OK line; it printed: " + (($lines | Select-Object -First 3) -join ' / ')
    }

    return $facts
}

function Clear-Scratch
{
    <#
        Tidy up on the way out of an early exit.

        An empty directory is litter and goes. A directory with renders in it
        stays and gets named: a sweep that stopped part way through leaves the
        audio it did produce, and that audio is usually the fastest way to find
        out why it stopped. A directory the caller named with -OutputDir is
        never removed either way; it is theirs.
    #>
    param([string]$Path, [bool]$Ours)

    if (-not [System.IO.Directory]::Exists($Path)) { return }

    $contents = @([System.IO.Directory]::EnumerateFileSystemEntries($Path))

    if ($Ours -and $contents.Count -eq 0)
    {
        try { [System.IO.Directory]::Delete($Path, $true) } catch { }
        return
    }

    Write-Output ''
    Write-Output ("Renders left in place: {0}" -f $Path)
}

function Get-SectionDeltas
{
    <# The DELTA lines score-sections.py prints, as a section -> dRMS map. #>
    param([string[]]$Lines)

    $deltas = @{}

    foreach ($line in $Lines)
    {
        if ($line -match '^DELTA\s+(\S+)\s+dRMS\s+([-+][0-9.]+)\s+dPeak\s+([-+][0-9.]+)')
        {
            $deltas[$Matches[1]] = [double]$Matches[2]
        }
    }

    return $deltas
}

#endregion

#region what the run needs before it starts

if ($Repeats -lt 1)
{
    Write-Output ("DENOISER AB TOOL FAIL: -Repeats must be at least 1, got {0}" -f $Repeats)
    exit 2
}

if ($Candidates.Count -lt 1)
{
    Write-Output 'DENOISER AB TOOL FAIL: -Candidates is empty; there is nothing to render'
    exit 2
}

if ($AudibleDb -le 0.0)
{
    Write-Output ("DENOISER AB TOOL FAIL: -AudibleDb must be above 0, got {0}" -f $AudibleDb)
    exit 2
}

$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

# Every path, not just the one for this platform: the usual reason for landing
# here is having built a different preset from the one being looked for, and
# that is only obvious with the list in front of you.
if (-not $exe)
{
    Write-Output 'DENOISER AB TOOL FAIL: no build found. Tried:'

    foreach ($candidate in $exeCandidates) { Write-Output ("  {0}" -f $candidate) }

    Write-Output 'build first: cmake --build build/release --config Release   (Windows, VS)'
    Write-Output '         or: cmake --build build/ninja-release              (Linux, macOS)'
    exit 2
}

if (-not $Take) { $Take = Join-Path $PSScriptRoot 'voice_test.wav' }

# No take, no sweep, and nothing synthesised to stand in for one. A generated
# tone or a noise file would render, score, and produce a table of numbers that
# looked exactly like a result -- and a denoiser trained on speech does
# something to a cello or a sine that has no bearing at all on what it does to a
# voice. A fabricated take is worse than no take, because a missing file is
# obvious and a wrong number is not.
if (-not (Test-Path -LiteralPath $Take))
{
    Write-Output ("DENOISER AB TOOL FAIL: no input take at {0}" -f $Take)
    Write-Output ''
    Write-Output 'Record one -- it takes 18 seconds:'
    Write-Output ('  tools/voice-headroom.sh "Microphone (USB audio CODEC)" "{0}"' -f $Take)
    Write-Output ''
    Write-Output 'It records quiet first and speech second in one take, which is what makes'
    Write-Output 'the two halves scoreable apart: the quiet half is where noise reduction is'
    Write-Output 'measured and the speech half is where the damage to your voice is. List the'
    Write-Output 'device names with: ffmpeg -list_devices true -f dshow -i dummy'
    Write-Output ''
    Write-Output 'Then check the take is usable before sweeping on it:'
    Write-Output ('  python tools/analyse-voice-headroom.py "{0}"' -f $Take)
    Write-Output ''
    Write-Output 'tools/*.wav is gitignored, so a fresh clone never has one and there is'
    Write-Output 'nothing to copy. Nothing here will invent one for you.'
    exit 2
}

$wav = Read-WavFacts -Path $Take

if ($null -eq $wav)
{
    Write-Output ("DENOISER AB TOOL FAIL: {0} is not a PCM WAV this can read" -f $Take)
    exit 2
}

# numpy is what the scorer needs and the system python on a Windows box usually
# does not have it, so the interpreter is checked here rather than discovered
# through a traceback forty minutes into a sweep.
$pythonCandidates = @()
if ($Python)            { $pythonCandidates += $Python }
if ($env:TOOLBOX_PYTHON) { $pythonCandidates += $env:TOOLBOX_PYTHON }
$pythonCandidates += @('python3', 'python')

$python = $null

foreach ($candidate in $pythonCandidates)
{
    try
    {
        $null = & $candidate -c 'import numpy' 2>&1

        if ($LASTEXITCODE -eq 0) { $python = $candidate; break }
    }
    catch { }
}

if (-not $python)
{
    Write-Output 'DENOISER AB TOOL FAIL: no python with numpy. Tried:'

    foreach ($candidate in $pythonCandidates) { Write-Output ("  {0}" -f $candidate) }

    Write-Output 'Pass one with -Python, or point TOOLBOX_PYTHON at an interpreter that has numpy.'
    exit 2
}

if (-not (Test-Path -LiteralPath $scorer))
{
    Write-Output ("DENOISER AB TOOL FAIL: the scorer is missing: {0}" -f $scorer)
    exit 2
}

if (-not (Test-Path -LiteralPath $loadScript) -and (@($LoadLevels | Where-Object { $_ -gt 0 }).Count -gt 0))
{
    Write-Output ("DENOISER AB TOOL FAIL: the load generator is missing: {0}" -f $loadScript)
    exit 2
}

if ($TimeoutSeconds -le 0) { $TimeoutSeconds = [int][Math]::Ceiling($wav.Seconds * 3 + 90) }

# Only a directory this script made is a directory this script may delete. A
# -OutputDir the caller named is theirs, and cleaning it up would be a tool
# removing files nobody asked it to touch.
$outputDirIsOurs = -not $OutputDir

if (-not $OutputDir)
{
    $OutputDir = Join-Path ([System.IO.Path]::GetTempPath()) ("denoiser-ab-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$null = [System.IO.Directory]::CreateDirectory($OutputDir)

$sectionArgs = @()
foreach ($section in $Sections) { $sectionArgs += @('--section', $section) }

#endregion

#region the sweep

$renderCount = $Candidates.Count * $LoadLevels.Count * $Repeats
$perRender   = $wav.Seconds + 8.0     # measured: about 5 s of startup, plus load ramp and teardown
$estimate    = ($renderCount + $(if ($DryChain) { 1 } else { 0 })) * $perRender / 60.0

Write-Output ("build            : {0}" -f $exe)
Write-Output ("take             : {0}" -f $Take)
Write-Output ("                   {0:F1} s, {1} Hz, {2} ch, {3}-bit" -f $wav.Seconds, $wav.SampleRate, $wav.Channels, $wav.Bits)
Write-Output ("python           : {0}" -f $python)
Write-Output ("candidates       : {0}" -f ($Candidates -join ', '))
Write-Output ("load levels      : {0} busy threads, on {1} logical cores" -f ($LoadLevels -join ', '), [Environment]::ProcessorCount)
Write-Output ("repeats          : {0}" -f $Repeats)
Write-Output ("renders          : {0}, paced, so about {1:F0} minutes" -f $renderCount, $estimate)
Write-Output ("renders kept in  : {0}" -f $OutputDir)
Write-Output ''

$activeLoad = $null
$results    = New-Object System.Collections.Generic.List[object]
$dryFile    = $Take
$dryLatency = 0.0

try
{
    if ($DryChain)
    {
        $dryFile = Join-Path $OutputDir 'dry-reference.wav'
        Write-Output ("dry reference    : rendering through '{0}'..." -f $DryChain)

        $dryFacts = Invoke-Render -Exe $exe -InputFile $Take -OutputFile $dryFile -Chain $DryChain `
                                  -TimeoutSeconds $TimeoutSeconds -ScratchDir $OutputDir

        if (-not $dryFacts.Ok)
        {
            Write-Output ("DENOISER AB TOOL FAIL: the dry reference render failed: {0}" -f $dryFacts.Message)
            Clear-Scratch -Path $OutputDir -Ours $outputDirIsOurs
            exit 2
        }

        $dryLatency = $dryFacts.LatencyMs
        Write-Output ("                   {0} ({1:F2} ms declared)" -f $dryFile, $dryLatency)
    }
    else
    {
        Write-Output 'dry reference    : the take itself (no plugin means unity gain; see the header)'
    }

    Write-Output ''

    foreach ($candidate in $Candidates)
    {
        $safe = ($candidate -replace '[^A-Za-z0-9]+', '-').Trim('-')

        foreach ($load in $LoadLevels)
        {
            for ($repeat = 1; $repeat -le $Repeats; $repeat++)
            {
                $outFile = Join-Path $OutputDir ("{0}-load{1:d2}-r{2:d2}.wav" -f $safe, $load, $repeat)

                $loadStarted = Get-Date
                $activeLoad  = Start-CpuLoad -Script $loadScript -Threads $load -Seconds ($TimeoutSeconds + 30)

                # A second for the threads to be running before the render
                # starts, so the first blocks see the load the rest of them do.
                if ($activeLoad) { Start-Sleep -Seconds 1 }

                $facts = Invoke-Render -Exe $exe -InputFile $Take -OutputFile $outFile -Chain $candidate `
                                       -TimeoutSeconds $TimeoutSeconds -ScratchDir $OutputDir

                $coresDelivered = Stop-CpuLoad -Process $activeLoad -StartedAt $loadStarted
                $activeLoad = $null

                $deltas = @{}

                if ($facts.Ok -and [System.IO.File]::Exists($outFile))
                {
                    # Formatted invariantly: a comma decimal separator on a
                    # non-English machine would reach argparse as text it
                    # cannot parse, forty minutes into a sweep.
                    $offsetMs = [Math]::Max(0.0, $facts.LatencyMs - $dryLatency)
                    $offsetText = $offsetMs.ToString('F3', [System.Globalization.CultureInfo]::InvariantCulture)

                    $scoreArgs = @($scorer, $dryFile, $outFile,
                                   '--wet-offset-ms', $offsetText,
                                   '--label', ("{0} at {1} threads, repeat {2}" -f $candidate, $load, $repeat)) + $sectionArgs

                    $scoreLines = @(& $python @scoreArgs 2>&1 | ForEach-Object { [string]$_ })

                    if ($LASTEXITCODE -ne 0)
                    {
                        $facts.Ok = $false
                        $facts.Message = "scoring failed: " + ($scoreLines -join ' ')
                    }
                    else
                    {
                        $deltas = Get-SectionDeltas -Lines $scoreLines
                    }
                }

                $results.Add([pscustomobject]@{
                    Candidate      = $candidate
                    Load           = $load
                    Repeat         = $repeat
                    Ok             = $facts.Ok
                    Message        = $facts.Message
                    RealtimeFactor = $facts.RealtimeFactor
                    BlocksBehind   = $facts.BlocksBehind
                    BlocksTotal    = $facts.BlocksTotal
                    BlocksKnown    = $facts.BlocksTotalKnown
                    LatencyMs      = $facts.LatencyMs
                    Warnings       = $facts.Warnings
                    CoresDelivered = $coresDelivered
                    WallSeconds    = $facts.WallSeconds
                    Deltas         = $deltas
                })

                $summary = if ($facts.Ok) {
                    ($deltas.Keys | Sort-Object | ForEach-Object { '{0} {1:F2}' -f $_, $deltas[$_] }) -join '  '
                } else {
                    'FAILED: ' + $facts.Message
                }

                Write-Output ("  {0,-14} {1,2} threads ({2,5:F2} cores)  repeat {3}/{4}  rt {5,5:F3}  behind {6,4}  {7}" -f `
                              $candidate, $load, $coresDelivered, $repeat, $Repeats,
                              $facts.RealtimeFactor, $facts.BlocksBehind, $summary)
            }
        }
    }
}
finally
{
    # Belt to cpu-load.ps1's own braces. A sweep interrupted between starting
    # the load and stopping it must not leave the machine pegged.
    $null = Stop-CpuLoad -Process $activeLoad -StartedAt (Get-Date)
}

#endregion

#region what it found

$failed = @($results | Where-Object { -not $_.Ok })

Write-Output ''

if ($failed.Count -gt 0)
{
    Write-Output ("DENOISER AB TOOL FAIL: {0} of {1} renders did not produce a score" -f $failed.Count, $results.Count)

    foreach ($failure in $failed)
    {
        Write-Output ("  {0,-14} {1,2} threads  repeat {2}  {3}" -f $failure.Candidate, $failure.Load, $failure.Repeat, $failure.Message)
    }

    Write-Output ''
    Write-Output 'No verdict: a sweep with holes in it cannot be compared against one without.'
    Clear-Scratch -Path $OutputDir -Ours $outputDirIsOurs
    exit 2
}

# Only sections every run scored. score-sections.py skips a section it cannot
# fit inside the audio, and averaging a column that some runs are missing from
# would quietly compare different amounts of the take.
$sectionNames = @($results[0].Deltas.Keys | Sort-Object | Where-Object {
    $name = $_
    @($results | Where-Object { -not $_.Deltas.ContainsKey($name) }).Count -eq 0
})

if ($sectionNames.Count -eq 0)
{
    Write-Output 'DENOISER AB TOOL FAIL: no section was scored on every run, so there is no'
    Write-Output 'column that means the same thing across the table. Check the take is long'
    Write-Output 'enough for the sections being asked for.'
    Clear-Scratch -Path $OutputDir -Ours $outputDirIsOurs
    exit 2
}

$groups = $results | Group-Object Candidate, Load

Write-Output 'NOISE REDUCTION ACHIEVED, per section, against the dry reference.'
Write-Output 'Negative is level removed. Range is the spread across repeats, and it is the'
Write-Output 'number that decides whether any difference below is real.'
Write-Output ''
Write-Output ("{0,-8} {1,-14} {2,7} {3,7} {4,3} {5,8} {6,8} {7,8} {8,8}" -f `
              'section', 'candidate', 'threads', 'cores', 'n', 'median', 'min', 'max', 'range')

foreach ($section in $sectionNames)
{
    foreach ($group in $groups)
    {
        $rows   = @($group.Group)
        $values = @($rows | ForEach-Object { [double]$_.Deltas[$section] })
        $stats  = $values | Measure-Object -Minimum -Maximum
        $cores  = Get-Median -Values @($rows | ForEach-Object { [double]$_.CoresDelivered })

        Write-Output ("{0,-8} {1,-14} {2,7} {3,7:F2} {4,3} {5,8:F2} {6,8:F2} {7,8:F2} {8,8:F2}" -f `
                      $section, $rows[0].Candidate, $rows[0].Load, $cores, $values.Count,
                      (Get-Median -Values $values), $stats.Minimum, $stats.Maximum,
                      ($stats.Maximum - $stats.Minimum))
    }
}

Write-Output ''
Write-Output 'HOW THE RENDER ITSELF HELD UP. blocksTotal is only printed by the render when'
Write-Output 'it fell behind, so a dash there means no block was late and the total was'
Write-Output 'never stated -- not that it is zero.'
Write-Output ''
Write-Output ("{0,-14} {1,7} {2,9} {3,9} {4,8} {5,12} {6,9}" -f `
              'candidate', 'threads', 'rt median', 'rt worst', 'behind', 'of blocks', 'warnings')

foreach ($group in $groups)
{
    $rows    = @($group.Group)
    $factors = @($rows | ForEach-Object { [double]$_.RealtimeFactor })
    $worst   = ($rows | Measure-Object -Property BlocksBehind -Maximum).Maximum
    $known   = @($rows | Where-Object { $_.BlocksKnown })
    $total   = if ($known.Count -gt 0) { '{0}' -f ($known | Measure-Object -Property BlocksTotal -Maximum).Maximum } else { '-' }
    $warned  = @($rows | Where-Object { $_.Warnings.Count -gt 0 }).Count

    Write-Output ("{0,-14} {1,7} {2,9:F3} {3,9:F3} {4,8} {5,12} {6,9}" -f `
                  $rows[0].Candidate, $rows[0].Load,
                  (Get-Median -Values $factors), ($factors | Measure-Object -Maximum).Maximum,
                  $worst, $total, $warned)
}

$warnings = @($results | Where-Object { $_.Warnings.Count -gt 0 })

if ($warnings.Count -gt 0)
{
    Write-Output ''
    Write-Output 'The render warned on some runs. A warning about pacing or a rejected plugin'
    Write-Output 'state means that run measured something other than the configuration under'
    Write-Output 'test, so the medians above are not clean:'

    foreach ($warned in $warnings)
    {
        foreach ($text in $warned.Warnings)
        {
            Write-Output ("  {0,-14} {1,2} threads repeat {2}: {3}" -f $warned.Candidate, $warned.Load, $warned.Repeat, $text)
        }
    }
}

#endregion

#region the verdict, or the refusal to give one

Write-Output ''

$verdictSection = if ($sectionNames -contains 'quiet') { 'quiet' } else { $sectionNames[0] }
$lightest = ($LoadLevels | Measure-Object -Minimum).Minimum
$heaviest = ($LoadLevels | Measure-Object -Maximum).Maximum

Write-Output ("Verdict is read from the '{0}' section: noise reduction is what a denoiser is" -f $verdictSection)
Write-Output ("for, and losing it under load is what DECISIONS.md's reversal trigger is about.")
Write-Output ''

if ($KeepRenders -or -not $outputDirIsOurs)
{
    Write-Output ("Renders kept: {0}" -f $OutputDir)
    Write-Output ''
}
else
{
    try { [System.IO.Directory]::Delete($OutputDir, $true) } catch { }
}

if ($LoadLevels.Count -lt 2 -or $Repeats -lt 2)
{
    Write-Output 'DENOISER AB TOOL FAIL: this configuration cannot answer the question.'
    Write-Output ("  load levels : {0} (two or more are needed to see an effect OF load)" -f $LoadLevels.Count)
    Write-Output ("  repeats     : {0} (two or more are needed to know the spread)" -f $Repeats)
    Write-Output ''
    Write-Output 'Every render above succeeded and every score is real; there is simply'
    Write-Output 'nothing here to compare them against. The defaults do answer it.'
    exit 2
}

$verdicts   = New-Object System.Collections.Generic.List[object]
$worstSpread = 0.0

foreach ($candidate in $Candidates)
{
    $light = @($results | Where-Object { $_.Candidate -eq $candidate -and $_.Load -eq $lightest } |
                ForEach-Object { [double]$_.Deltas[$verdictSection] })
    $heavy = @($results | Where-Object { $_.Candidate -eq $candidate -and $_.Load -eq $heaviest } |
                ForEach-Object { [double]$_.Deltas[$verdictSection] })

    $lightRange = (($light | Measure-Object -Maximum).Maximum - ($light | Measure-Object -Minimum).Minimum)
    $heavyRange = (($heavy | Measure-Object -Maximum).Maximum - ($heavy | Measure-Object -Minimum).Minimum)
    $spread     = [Math]::Max($lightRange, $heavyRange)

    # dRMS is negative when level was removed, so a LESS negative median under
    # load means less noise taken out: the degradation is the difference in
    # that direction.
    $lost = (Get-Median -Values $heavy) - (Get-Median -Values $light)

    if ($spread -gt $worstSpread) { $worstSpread = $spread }

    $verdicts.Add([pscustomobject]@{ Candidate = $candidate; Lost = $lost; Spread = $spread })

    Write-Output ("{0,-14} lost {1,6:F2} dB between {2} and {3} threads, against a spread of {4:F2} dB" -f `
                  $candidate, $lost, $lightest, $heaviest, $spread)
}

Write-Output ''

$degraded = @($verdicts | Where-Object { $_.Lost -ge $AudibleDb -and $_.Lost -gt $_.Spread })

if ($degraded.Count -gt 0)
{
    Write-Output 'DENOISER AB FAIL'

    foreach ($verdict in $degraded)
    {
        Write-Output ("  {0} loses {1:F2} dB of noise reduction at {2} busy threads, which is more than" -f `
                      $verdict.Candidate, $verdict.Lost, $heaviest)
        Write-Output ("  the {0:F2} dB the repeats disagree by and more than the {1:F2} dB called audible." -f `
                      $verdict.Spread, $AudibleDb)
    }

    Write-Output ''
    Write-Output 'This is the condition DECISIONS.md wrote down when it kept Salvor over Alt'
    Write-Output 'Denoiser: "revisit if artifacts are ever audible under load -- Alt Denoiser is'
    Write-Output 'provably immune, being synchronous." Revisit it, and put these numbers in the'
    Write-Output 'entry rather than a new opinion.'
    exit 1
}

# The house habit, and the reason this is a 2 and not a 0: a spread wider than
# the effect means the experiment could not have seen the effect. Reporting
# "no degradation" from that would be reporting the noise floor of the method
# as a result about the plugin.
if ($worstSpread -ge $AudibleDb)
{
    Write-Output 'DENOISER AB TOOL FAIL: no verdict. The repeats disagree with each other by'
    Write-Output ("{0:F2} dB, which is as large as the {1:F2} dB difference being looked for. A sweep" -f $worstSpread, $AudibleDb)
    Write-Output 'this noisy cannot tell a degraded candidate from a lucky run, and saying'
    Write-Output '"no degradation" from it would be reporting the method''s own noise as a'
    Write-Output 'result about the plugin.'
    Write-Output ''
    Write-Output ("  raise the repeats : -Repeats {0}" -f ($Repeats * 3))
    Write-Output '  or take a longer take, so each score averages over more audio'
    exit 2
}

Write-Output 'DENOISER AB OK'
Write-Output ("  No candidate lost {0:F2} dB or more of noise reduction between {1} and {2} busy" -f $AudibleDb, $lightest, $heaviest)
Write-Output ("  threads, and the repeats agreed to within {0:F2} dB, so that is a result rather" -f $worstSpread)
Write-Output '  than an absence of evidence.'
Write-Output ''

if ($Candidates.Count -ge 2)
{
    $first  = Get-Median -Values @($results | Where-Object { $_.Candidate -eq $Candidates[0] -and $_.Load -eq $heaviest } | ForEach-Object { [double]$_.Deltas[$verdictSection] })
    $second = Get-Median -Values @($results | Where-Object { $_.Candidate -eq $Candidates[1] -and $_.Load -eq $heaviest } | ForEach-Object { [double]$_.Deltas[$verdictSection] })
    $gap    = [Math]::Abs($first - $second)

    if ($gap -le $worstSpread)
    {
        Write-Output ("  {0} and {1} are indistinguishable at {2} threads: {3:F2} dB apart, which is" -f `
                      $Candidates[0], $Candidates[1], $heaviest, $gap)
        Write-Output ("  inside the {0:F2} dB the repeats disagree by. That is the expected answer -- both" -f $worstSpread)
        Write-Output '  wrap DeepFilterNet3 -- and it is not a reason to prefer either of them.'
    }
    else
    {
        Write-Output ("  {0} and {1} differ by {2:F2} dB at {3} threads, outside the {4:F2} dB spread." -f `
                      $Candidates[0], $Candidates[1], $gap, $heaviest, $worstSpread)
        Write-Output '  Both wrap DeepFilterNet3, so a real gap here is worth explaining before it is'
        Write-Output '  worth acting on.'
    }
}

exit 0

#endregion
