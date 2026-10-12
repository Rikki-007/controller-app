<#
.SYNOPSIS
    Builds and runs the unit tests (and optionally the end-to-end test) on
    Windows, coping with Smart App Control.

.DESCRIPTION
    Smart App Control (SAC) refuses to start unsigned executables that have
    no reputation with Microsoft. On a machine where it is enforcing, that
    includes every test binary this repo builds, and can include MSYS2's own
    compiler (cc1.exe). Execution policy and self-signing do not help: the
    policy governs only PowerShell scripts, and SAC trusts only certificates
    that chain to Microsoft's trusted roots.

    This script:
      1. builds natively with mingw32-make and runs each test binary;
      2. if SAC blocks the compiler or a test, runs the same Makefile inside
         WSL instead (Linux binaries are not subject to SAC), using a
         separate output directory, build/wsl;
      3. if there is no usable WSL distro, says so and lists the options.

    Run it without changing the system execution policy:

        powershell -ExecutionPolicy Bypass -File scripts\test.ps1 [-E2E] [-Wsl]

.PARAMETER E2E
    Also run tests/e2e.sh: the real vcd daemon driven by tools/vc_send.

.PARAMETER Wsl
    Skip the native attempt and go straight to WSL.

.NOTES
    Exit codes: 0 all passed, 1 a test or the build failed, 3 nothing could
    be verified because SAC blocked it and WSL was not available.
#>
param(
    [switch]$E2E,
    [switch]$Wsl
)

$Root = Split-Path -Parent $PSScriptRoot
$Tests = 'test_parser', 'test_hid_output', 'test_network', 'test_encoder', 'test_loopback',
         'test_protocol', 'test_session', 'test_layout'
$SacPattern = 'Application Control|cannot execute .*(cc1|as|ld|collect2)'

function Write-Step($Text) { Write-Host "== $Text" -ForegroundColor Cyan }

# Runs mingw32-make with the given targets. Returns 'ok', 'blocked' or 'failed'.
function Invoke-NativeMake([string[]]$Targets) {
    $make = Get-Command mingw32-make -ErrorAction SilentlyContinue
    if (-not $make) {
        Write-Host 'mingw32-make is not on PATH (MSYS2 UCRT64: C:\msys64\ucrt64\bin).'
        return 'failed'
    }
    $output = & $make.Source -C $Root @Targets 2>&1 | ForEach-Object { "$_" }
    $output | Write-Host
    if ($LASTEXITCODE -eq 0) { return 'ok' }
    if (($output -join "`n") -match $SacPattern) { return 'blocked' }
    return 'failed'
}

# Runs one built binary. Returns 'ok', 'blocked' or 'failed'.
function Invoke-Binary([string]$Path, [string[]]$Arguments) {
    try {
        & $Path @Arguments | Write-Host
        if ($LASTEXITCODE -eq 0) { return 'ok' }
        return 'failed'
    } catch {
        if ($_.Exception.Message -match $SacPattern) {
            Write-Host "  blocked by Smart App Control: $Path" -ForegroundColor Yellow
            return 'blocked'
        }
        Write-Host "  could not run ${Path}: $($_.Exception.Message)" -ForegroundColor Red
        return 'failed'
    }
}

function Test-Native {
    Write-Step 'Native build (MinGW)'
    $targets = @('test-build')
    if ($E2E) { $targets += 'all' }
    $built = Invoke-NativeMake $targets
    if ($built -ne 'ok') { return $built }

    Write-Step 'Native unit tests'
    $blocked = 0
    $failed = 0
    foreach ($t in $Tests) {
        Write-Host "-- $t"
        $result = Invoke-Binary (Join-Path $Root "build\test\$t.exe") @()
        if ($result -eq 'blocked') { $blocked++ }
        if ($result -eq 'failed') { $failed++ }
    }
    if ($failed -gt 0) { return 'failed' }
    if ($blocked -gt 0) { return 'blocked' }

    if ($E2E) {
        Write-Step 'Native end-to-end test'
        $sh = Get-Command sh -ErrorAction SilentlyContinue
        if (-not $sh) {
            Write-Host 'sh is not on PATH (Git for Windows or MSYS2 provides it).'
            return 'failed'
        }
        Push-Location $Root
        try {
            & $sh.Source tests/e2e.sh build/stub/vcd build/tools/vc_send | Write-Host
            $code = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        # e2e.sh prints vcd's output; a blocked daemon shows up as "did not start".
        if ($code -ne 0) { return 'failed' }
    }
    return 'ok'
}

# First installed WSL distro, or $null. wsl.exe writes UTF-16, so strip NULs.
function Get-WslDistro {
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return $null }
    $names = & $wsl.Source --list --quiet 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    foreach ($n in $names) {
        $clean = ("$n" -replace "`0", '').Trim()
        if ($clean -and $clean -notmatch '^docker-desktop') { return $clean }
    }
    return $null
}

function Test-Wsl {
    $distro = Get-WslDistro
    if (-not $distro) { return 'unavailable' }

    Write-Step "WSL ($distro): build and unit tests in build/wsl"
    $targets = 'make BUILD=build/wsl test'
    if ($E2E) { $targets += ' && make BUILD=build/wsl e2e' }
    $check = 'command -v make >/dev/null && command -v gcc >/dev/null || ' +
             '{ echo "WSL needs a compiler: sudo apt install build-essential"; exit 1; }'
    & wsl.exe -d $distro --cd $Root -- sh -c "$check; $targets" | Write-Host
    if ($LASTEXITCODE -eq 0) { return 'ok' }
    return 'failed'
}

function Show-Options {
    Write-Host @'

Smart App Control blocked the build or the tests, and no WSL distro is set up,
so nothing was verified on this machine. Options, most contained first:

  1. WSL (recommended). Linux binaries are not subject to SAC, and the POSIX
     code paths get built too. Once, from an elevated prompt (may reboot):
         wsl --install -d Ubuntu
     then inside Ubuntu:  sudo apt update && sudo apt install -y build-essential
     and rerun this script.

  2. CI. Push a branch: .github/workflows/ci.yml builds and runs everything
     on GitHub's Windows and Linux runners, which do not use SAC.

  3. Turn Smart App Control off (Windows Security > App & browser control >
     Smart App Control settings). It applies to the whole machine, and on some
     Windows builds it cannot be turned back on without reinstalling Windows;
     read the warning on that page first.

Not options: Set-ExecutionPolicy (affects scripts only, not .exe files) and
self-signed certificates (SAC only trusts certificates that chain to
Microsoft's trusted roots).
'@
}

$native = 'blocked'
if (-not $Wsl) {
    $native = Test-Native
    if ($native -eq 'ok') {
        Write-Step 'All native tests passed'
        exit 0
    }
    if ($native -eq 'failed') {
        Write-Step 'FAILED (native)'
        exit 1
    }
    Write-Host 'Smart App Control blocked the native run; trying WSL.' -ForegroundColor Yellow
}

$viaWsl = Test-Wsl
if ($viaWsl -eq 'ok') {
    Write-Step 'All tests passed (WSL)'
    exit 0
}
if ($viaWsl -eq 'failed') {
    Write-Step 'FAILED (WSL)'
    exit 1
}
Show-Options
exit 3
