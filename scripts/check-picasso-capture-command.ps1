#Requires -Version 5.1
param([Parameter(Mandatory = $true)][string]$Output)
$ErrorActionPreference = 'Stop'
$taskFixtureOutput = $Output
. (Join-Path $PSScriptRoot 'capture-picasso-usb-console.ps1')
Add-Type -Path (Join-Path $PSScriptRoot 'picasso-windows-serial.cs')

# Exercise the production capture loop with a disconnecting command transport.
# No COM port is opened and no device receives commands.
class CaptureCommandFixture {
    static [int]$Sent
    static [int]$Opened
    static [bool]$Disconnected
    static [bool]$FailSend
    static [string]$Log
    [int] Read([byte[]]$Buffer, [int]$Timeout) {
        Start-Sleep -Milliseconds 25
        if ([CaptureCommandFixture]::Sent -gt 0 -and -not [CaptureCommandFixture]::Disconnected) {
            [CaptureCommandFixture]::Disconnected = $true
            throw 'simulated device disconnect'
        }
        $bytes = [Text.Encoding]::ASCII.GetBytes("fixture log before command`r`n")
        [Array]::Copy($bytes, $Buffer, $bytes.Length)
        return $bytes.Length
    }
    [void] SendDiagnosticCommand([string]$Command) {
        if ((Get-Item -LiteralPath ([CaptureCommandFixture]::Log)).Length -eq 0) {
            throw 'capture must save bytes before sending a command'
        }
        [CaptureCommandFixture]::Sent++
        if ([CaptureCommandFixture]::FailSend) { throw 'simulated partial command write' }
    }
    [void] Dispose() { }
}
function Get-PicassoPorts {
    [pscustomobject]@{ Port = 'COM99'; PNPDeviceID = 'USB\VID_1209&PID_0001\PICASSO01'; Name = 'fixture' }
}
function Open-CapturePort {
    param($Device, [switch]$ConfigureLine)
    [CaptureCommandFixture]::Opened++
    return [CaptureCommandFixture]::new()
}
function Test-CapturePortPresent { param([string]$Port) return $true }

$taskOutput = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($taskFixtureOutput)
New-Item -ItemType Directory -Path $taskOutput -ErrorAction Stop | Out-Null
$results = @()
foreach ($case in @('default', 'panic-once', 'partial-write')) {
    [CaptureCommandFixture]::Sent = 0
    [CaptureCommandFixture]::Opened = 0
    [CaptureCommandFixture]::Disconnected = $false
    [CaptureCommandFixture]::FailSend = $case -eq 'partial-write'
    [CaptureCommandFixture]::Log = Join-Path $taskOutput "$case.log"
    $command = if ($case -eq 'default') { '' } else { 'panic-test' }
    $rc = Invoke-PicassoCapture -Output ([CaptureCommandFixture]::Log) -Duration 1 -Wait 2 `
        -NoStdout -DeviceCommand $command -CommandAfterSeconds 0
    $expected = if ($case -eq 'default') { 0 } else { 1 }
    if ($rc -ne 0 -or [CaptureCommandFixture]::Sent -ne $expected) { throw "$case failed: rc=$rc" }
    if ($expected -and [CaptureCommandFixture]::Opened -lt 2) { throw "$case did not exercise reconnect" }
    $results += [pscustomobject]@{ case = $case; passed = $true; sends = [CaptureCommandFixture]::Sent; opens = [CaptureCommandFixture]::Opened }
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'results.json') -Encoding UTF8
'PASS: capture defaults to read-only, saves before sending, and never resends across disconnect/partial write'
