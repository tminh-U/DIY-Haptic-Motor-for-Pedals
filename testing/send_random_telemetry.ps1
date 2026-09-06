param(
    [string]$Port = "COM5",
    [int]$Baud = 115200,
    [ValidateRange(1, 60)]
    [double]$DurationMinutes = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PacketPeriod = 1.0 / 60.0
$StressDurationSeconds = $DurationMinutes * 60.0
$Culture = [Globalization.CultureInfo]::InvariantCulture
$Rng = [Random]::new()

function Get-Between([double]$Low, [double]$High) {
    $Low + ($High - $Low) * $Rng.NextDouble()
}

function Get-BiasedHigh([double]$Low, [double]$High) {
    $Low + ($High - $Low) * [Math]::Sqrt($Rng.NextDouble())
}

function New-Profile {
    $Pick = $Rng.NextDouble()

    # Favor ABS and slip: these are the highest useful RMS-load paths through
    # the original mix table, while ROAD/COMBINED still get exercised.
    if ($Pick -lt 0.50) {
        [PSCustomObject]@{ Name = "ABS"; Duration = Get-Between 0.50 1.80 }
    } elseif ($Pick -lt 0.75) {
        [PSCustomObject]@{ Name = "SLIP"; Duration = Get-Between 0.30 1.20 }
    } elseif ($Pick -lt 0.90) {
        [PSCustomObject]@{ Name = "ROAD"; Duration = Get-Between 0.25 0.80 }
    } else {
        [PSCustomObject]@{ Name = "COMBINED"; Duration = Get-Between 0.30 1.00 }
    }
}

function New-Packet([string]$Profile) {
    switch ($Profile) {
        "ABS" {
            # Hard braking around ABS intervention/recovery.
            [double[]]@(
                1.0,
                (Get-BiasedHigh 0.02 0.16),
                (Get-BiasedHigh 0.02 0.16),
                (Get-Between 0.0 0.008),
                (Get-Between 0.0 0.008)
            )
            break
        }
        "SLIP" {
            $Severe = Get-BiasedHigh 0.45 1.10
            $Moderate = Get-BiasedHigh 0.12 0.65
            if ($Rng.Next(2) -eq 0) {
                $SlipL, $SlipR = $Severe, $Moderate
            } else {
                $SlipL, $SlipR = $Moderate, $Severe
            }
            [double[]]@(
                0.0, $SlipL, $SlipR,
                (Get-Between 0.0 0.008),
                (Get-Between 0.0 0.008)
            )
            break
        }
        "ROAD" {
            $Severe = Get-BiasedHigh 0.55 1.00
            $Moderate = Get-BiasedHigh 0.20 0.70
            if ($Rng.Next(2) -eq 0) {
                $RoadL, $RoadR = $Severe, $Moderate
            } else {
                $RoadL, $RoadR = $Moderate, $Severe
            }
            [double[]]@(
                0.0,
                (Get-Between 0.0 0.025),
                (Get-Between 0.0 0.025),
                $RoadL, $RoadR
            )
            break
        }
        default {
            $Abs = if ($Rng.NextDouble() -lt 0.85) { 1.0 } else { 0.0 }
            [double[]]@(
                $Abs,
                (Get-BiasedHigh 0.25 1.05),
                (Get-BiasedHigh 0.25 1.05),
                (Get-BiasedHigh 0.35 0.90),
                (Get-BiasedHigh 0.35 0.90)
            )
        }
    }
}

function ConvertTo-Line([double[]]$Values) {
    $Fields = foreach ($Value in $Values) {
        $Value.ToString("F4", $Culture)
    }
    $Fields -join ","
}

function Send-Packet(
    [IO.Ports.SerialPort]$SerialPort,
    [double[]]$Values
) {
    $SerialPort.WriteLine((ConvertTo-Line $Values))
}

$Esp = [IO.Ports.SerialPort]::new(
    $Port, $Baud, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One
)
$Esp.NewLine = [string][char]10
$Esp.ReadTimeout = 800
$Esp.WriteTimeout = 500
$Esp.DtrEnable = $true
$Esp.RtsEnable = $true

try {
    $Esp.Open()
    Write-Host "Opened $Port; waiting for ESP32 boot..."
    Start-Sleep -Seconds 2

    $Esp.DiscardInBuffer()
    $Esp.WriteLine("ID?")
    try {
        $Identity = $Esp.ReadLine().Trim()
        if ($Identity.StartsWith("HAPTIC_PEDAL,1,")) {
            Write-Host "Connected: $Identity"
        } else {
            Write-Warning "Unexpected identity response: $Identity"
        }
    } catch [TimeoutException] {
        Write-Warning "No HAPTIC_PEDAL identity response received."
    }

    Send-Packet $Esp ([double[]]@(0, 0, 0, 0, 0))

    $Clock = [Diagnostics.Stopwatch]::StartNew()
    $Running = $false
    $Quit = $false
    $ActiveSince = 0.0
    $NextPacketAt = 0.0
    $Profile = New-Profile
    $ProfileEndsAt = 0.0
    $PrintDivider = 0

    Write-Host "Press 1 to start, 0 to stop, Q to quit."
    Write-Host "Uses the original firmware UART/parser/watchdog/waveforms."
    Write-Host "Continuous stress duration: $DurationMinutes minute(s), no cooldown."

    while (-not $Quit) {
        while ([Console]::KeyAvailable) {
            $Key = [char]::ToLowerInvariant([Console]::ReadKey($true).KeyChar)

            if ($Key -eq "1") {
                $Running = $true
                $ActiveSince = $Clock.Elapsed.TotalSeconds
                $NextPacketAt = $ActiveSince
                $ProfileEndsAt = 0.0
                Write-Host "TEST ON"
            } elseif ($Key -eq "0") {
                $Running = $false
                Send-Packet $Esp ([double[]]@(0, 0, 0, 0, 0))
                Write-Host "TEST OFF"
            } elseif ($Key -eq "q") {
                $Quit = $true
            }
        }

        if ($Quit) { break }
        if (-not $Running) {
            Start-Sleep -Milliseconds 5
            continue
        }

        $Now = $Clock.Elapsed.TotalSeconds

        if (($Now - $ActiveSince) -ge $StressDurationSeconds) {
            $Running = $false
            Send-Packet $Esp ([double[]]@(0, 0, 0, 0, 0))
            Write-Host "STRESS TEST COMPLETE: $DurationMinutes minute(s)"
            continue
        }

        if ($Now -lt $NextPacketAt) {
            Start-Sleep -Milliseconds 1
            continue
        }

        if ($Now -ge $ProfileEndsAt) {
            $Profile = New-Profile
            $ProfileEndsAt = $Now + $Profile.Duration
        }

        $Values = New-Packet $Profile.Name
        Send-Packet $Esp $Values
        $NextPacketAt += $PacketPeriod

        # Never burst stale frames after a Windows scheduling delay.
        if (($Now - $NextPacketAt) -gt ($PacketPeriod * 2)) {
            $NextPacketAt = $Now + $PacketPeriod
        }

        $PrintDivider++
        if ($PrintDivider -ge 6) {
            $PrintDivider = 0
            Write-Host ("[{0}] {1}" -f $Profile.Name, (ConvertTo-Line $Values))
        }
    }
} finally {
    if ($Esp.IsOpen) {
        try {
            Send-Packet $Esp ([double[]]@(0, 0, 0, 0, 0))
        } catch {
            # Port may already be disconnected.
        }
        $Esp.Close()
    }
    $Esp.Dispose()
}
