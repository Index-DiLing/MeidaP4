#Requires -Version 5.1
<#
.SYNOPSIS
    MeidaP4 probe firmware - flash (and optionally monitor) one command.

.DESCRIPTION
    Flashing needs a real board. If the ESP32-P4 is NOT attached, this script fails with a
    list of the serial ports it can see instead of guessing.

    Port auto-detection matches Espressif / CP210x / CH34x / FTDI / USB-Serial-JTAG by their
    Windows device friendly name. The P4 high-speed OTG port is NOT a serial port, so the
    flashing port is normally a separate USB-Serial-JTAG or UART bridge interface.

.EXAMPLE
    pwsh -File tools/flash.ps1 -Mode a -Slot main
    pwsh -File tools/flash.ps1 -Mode b -Slot main -Port COM5 -NoMonitor
#>
[CmdletBinding()]
param(
    [ValidateSet('a', 'b', 'prod')] [string] $Mode = 'a',
    [string] $Slot = 'main',
    [string] $Port,
    # 921600 verified working on this board's CH343 bridge (266 KB in 2.1 s).
    # The CH343 handles it; raise to 1500000 only if the cable/board is happy with it.
    [int] $Baud = 921600,
    [switch] $Build,
    [switch] $NoMonitor,
    [switch] $Quiet,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $IdfArgs
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'idf-env.ps1') -Quiet:$Quiet

$buildDir  = Join-Path $Root "build-$Slot-$Mode"
$sdkconfig = Join-Path $Root "sdkconfig.$Slot-$Mode"
$defaults  = "sdkconfig.defaults;sdkconfig.$Mode.defaults"

if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'flasher_args.json'))) {
    throw "[flash] no build found in '$buildDir'. Run: pwsh -File tools/build.ps1 -Mode $Mode -Slot $Slot"
}

function Get-ProbeSerialPort {
    $candidates = @()
    foreach ($name in [System.IO.Ports.SerialPort]::GetPortNames()) {
        $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object { $_.FriendlyName -like "*($name)*" } |
               Select-Object -First 1
        $label = if ($null -ne $dev) { $dev.FriendlyName } else { '(unknown)' }
        if ($label -match 'ESP|CP210|CH34|FTDI|Silicon Labs|USB Serial|JTAG|USB-Enhanced') {
            $candidates += [pscustomobject]@{ Port = $name; Label = $label }
        }
    }
    return $candidates
}

if (-not $Port) {
    $found = @(Get-ProbeSerialPort)
    if ($found.Count -eq 1) {
        $Port = $found[0].Port
        Write-Host "[flash] auto-detected $Port ($($found[0].Label))"
    } elseif ($found.Count -gt 1) {
        Write-Host "[flash] several candidate ports - pass -Port explicitly:"
        $found | Format-Table -AutoSize
        throw "[flash] ambiguous port"
    } else {
        Write-Host "[flash] visible serial ports: $([System.IO.Ports.SerialPort]::GetPortNames() -join ', ')"
        throw "[flash] no Espressif/USB-serial port found - is the board attached and powered?"
    }
}

Push-Location $Root
try {
    if ($Build) {
        Invoke-IdfPy -B $buildDir "-DSDKCONFIG=$sdkconfig" "-DSDKCONFIG_DEFAULTS=$defaults" @IdfArgs build
        if ($LASTEXITCODE -ne 0) { throw "[flash] build failed (exit $LASTEXITCODE)" }
    }

    Write-Host "[flash] flashing $Port @ $Baud ..."
    Invoke-IdfPy -B $buildDir "-DSDKCONFIG=$sdkconfig" "-DSDKCONFIG_DEFAULTS=$defaults" -p $Port -b $Baud @IdfArgs flash
    $code = $LASTEXITCODE

    if ($code -eq 0 -and -not $NoMonitor) {
        Write-Host "[flash] starting monitor (Ctrl+] to exit) ..."
        Invoke-IdfPy -B $buildDir -p $Port monitor
        $code = $LASTEXITCODE
    }
} finally {
    Pop-Location
}

exit $code
