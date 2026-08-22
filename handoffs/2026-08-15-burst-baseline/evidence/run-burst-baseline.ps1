# Baseline burst runs: native, OSSS motion, OSSS blend. One fixed configuration.
# Both executables are console apps; they are started with CreateNoWindow so no
# terminal window appears over the test animation and nothing overrides the
# apps' own ShowWindow calls.
param(
    [string]$Repo = 'C:\Users\user\OneDrive\Desktop\Formalities\OSSS',
    [string]$Out = 'C:\Users\user\AppData\Local\Temp\OSSS\burst-baseline',
    [string[]]$Cases = @('native', 'osss-motion', 'osss-blend')
)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Wf { [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title); }
"@
New-Item -ItemType Directory -Force $Out | Out-Null
$anim = Join-Path $Repo 'out\release\osss_test_animation.exe'
$osss = Join-Path $Repo 'out\release\osss.exe'
# The burst is scheduled at 26 s of animation time: osss.exe compiles its HLSL at
# startup and takes 10-15 s on this machine to bring its overlay up, so an early
# burst scores the bare source and osss then dies on the vanished target.
# 26 s is cycle phase 2.0 s (first half; the temporal search reaches back to
# 1.75 s, still clear of the scene cut at 3.0 s).
$animArgs = '--api d3d11 --fps 60 --burst 20 --burst-at-ms 26000 --exit-after-burst'
$osssBase = '--title "OSSS Test Animation" --target-fps 240 --max-multiplier 4 --stats-overlay off'

function Start-Quiet([string]$exe, [string]$arguments, [string]$outPath, [string]$errPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    return @{ Process = $p; OutTask = $outTask; ErrTask = $errTask; OutPath = $outPath; ErrPath = $errPath }
}

function Finish-Quiet($h) {
    $h.Process.WaitForExit()
    [System.IO.File]::WriteAllText($h.OutPath, $h.OutTask.Result, [System.Text.Encoding]::UTF8)
    [System.IO.File]::WriteAllText($h.ErrPath, $h.ErrTask.Result, [System.Text.Encoding]::UTF8)
}

function Run-Case([string]$name, [string]$interpolator) {
    Write-Host "=== $name ==="
    $animOut = Join-Path $Out "$name-animation-stdout.txt"
    $animErr = Join-Path $Out "$name-animation-stderr.txt"
    $started = Get-Date
    $a = Start-Quiet $anim $animArgs $animOut $animErr
    $j = $null
    if ($interpolator) {
        Start-Sleep -Milliseconds 1500
        $j = Start-Quiet $osss "$osssBase --interpolator $interpolator" (Join-Path $Out "$name-osss-stdout.txt") (Join-Path $Out "$name-osss-stderr.txt")
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $seen = $null
        while ($sw.Elapsed.TotalSeconds -lt 24 -and -not $j.Process.HasExited) {
            if ([Wf]::FindWindowW('OSSS.FrameOutput', [NullString]::Value) -ne [IntPtr]::Zero) { $seen = $sw.Elapsed.TotalSeconds; break }
            Start-Sleep -Milliseconds 100
        }
        Write-Host ("osss overlay window up {0} s after osss launch (animation t = {1:N1} s)" -f $(if ($seen) { '{0:N1}' -f $seen } else { 'NOT' }), ((Get-Date) - $started).TotalSeconds)
    }
    if (-not $a.Process.WaitForExit(60000)) { Write-Host "animation did not exit; killing"; $a.Process.Kill() }
    Finish-Quiet $a
    if ($j) {
        if (-not $j.Process.WaitForExit(8000)) { Write-Host "osss did not exit after target closed; killing"; $j.Process.Kill() }
        Finish-Quiet $j
    }
    $elapsed = ((Get-Date) - $started).TotalSeconds
    $jcode = if ($j) { $j.Process.ExitCode } else { 'n/a' }
    Write-Host ("animation exit={0} osss exit={1} elapsed={2:N1}s" -f $a.Process.ExitCode, $jcode, $elapsed)
    Get-Content $animOut | Where-Object { $_ -match '^burst ' } | ForEach-Object { Write-Host $_ }
    if ($j) { Get-Content (Join-Path $Out "$name-osss-stderr.txt") | ForEach-Object { Write-Host "osss stderr: $_" } }
    $line = Get-Content $animOut | Where-Object { $_ -match 'report (.+\.json)' } | Select-Object -Last 1
    if ($line -match 'report (.+\.json)') {
        $report = $Matches[1].Trim()
        Copy-Item $report (Join-Path $Out "$name-burst.json")
        Write-Host "copied $report"
    } else {
        Write-Host "no report line found"
    }
    Start-Sleep -Milliseconds 800
}

foreach ($case in $Cases) {
    switch ($case) {
        'native' { Run-Case 'native' $null }
        'osss-motion' { Run-Case 'osss-motion' 'motion' }
        'osss-blend' { Run-Case 'osss-blend' 'blend' }
    }
}
Write-Host "done"
