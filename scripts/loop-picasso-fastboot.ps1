[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BootImage,

    [string]$Serial = 'e42e512b',
    [string]$PlatformTools = 'C:\Users\34063\Downloads\platform-tools-latest-windows\platform-tools',
    [int]$MaxAttempts = 0,
    [switch]$FlashBoot
)

$ErrorActionPreference = 'Stop'
$fastboot = Join-Path $PlatformTools 'fastboot.exe'
$adb = Join-Path $PlatformTools 'adb.exe'

if (-not (Test-Path -LiteralPath $fastboot -PathType Leaf)) {
    throw "fastboot.exe not found: $fastboot"
}
if (-not (Test-Path -LiteralPath $BootImage -PathType Leaf)) {
    throw "Boot image not found: $BootImage"
}

function Test-FastbootDevice {
    $lines = @(& $fastboot devices 2>$null)
    foreach ($line in $lines) {
        if ($line -match ('^' + [regex]::Escape($Serial) + '\s+fastboot\s*$')) {
            return $true
        }
    }
    return $false
}

function Test-AdbDevice {
    if (-not (Test-Path -LiteralPath $adb -PathType Leaf)) {
        return $false
    }
    $lines = @(& $adb devices 2>$null)
    foreach ($line in $lines) {
        if ($line -match ('^' + [regex]::Escape($Serial) + '\s+device\s*$')) {
            return $true
        }
    }
    return $false
}

function Wait-ForFastboot {
    while (-not (Test-FastbootDevice)) {
        # If a previous boot reached Android instead of the bootloader, bring it
        # back to Fastboot without touching any partition.
        if (Test-AdbDevice) {
            Write-Host "[$(Get-Date -Format s)] ADB detected; requesting bootloader"
            & $adb -s $Serial reboot bootloader | Out-Host
            Start-Sleep -Seconds 2
        } else {
            Start-Sleep -Seconds 1
        }
    }
}

Write-Host "Picasso Fastboot loop for $Serial"
Write-Host "Image: $BootImage"
if ($FlashBoot) {
    Write-Warning 'Flash mode is enabled: this writes the image to the boot partition.'
    Wait-ForFastboot
    & $fastboot -s $Serial flash boot $BootImage | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "fastboot flash boot failed with exit code $LASTEXITCODE" }
    & $fastboot -s $Serial reboot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "fastboot reboot failed with exit code $LASTEXITCODE" }
}

$attempt = 0
while ($MaxAttempts -le 0 -or $attempt -lt $MaxAttempts) {
    Wait-ForFastboot
    $attempt++
    Write-Host "[$(Get-Date -Format s)] attempt ${attempt}: starting temporary boot"

    if ($FlashBoot) {
        # The flashed boot partition is already selected; a normal reboot from
        # Fastboot exercises that partition and lets the kernel arm the
        # Fastboot reason again when its panic handler resets the phone.
        & $fastboot -s $Serial reboot | Out-Host
    } else {
        & $fastboot -s $Serial boot $BootImage | Out-Host
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Fastboot launch failed with exit code $LASTEXITCODE; retrying"
        Start-Sleep -Seconds 2
        continue
    }

    # Do not issue another boot command while the current image is running.
    # The kernel's panic/reset path should return the device to Fastboot.
    while (Test-FastbootDevice) { Start-Sleep -Seconds 1 }
    Write-Host "[$(Get-Date -Format s)] device left Fastboot; waiting for the next reset"
}
