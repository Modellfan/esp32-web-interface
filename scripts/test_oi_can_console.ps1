param(
    [string]$PortName = "COM23",
    [int]$BaudRate = 115200,
    [string]$ParameterName = "",
    [int]$OpenTimeoutMs = 12000,
    [int]$CommandTimeoutMs = 15000,
    [switch]$EnableTrace
)

$ErrorActionPreference = "Stop"

function Read-LinesUntil {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [ScriptBlock]$StopWhen,
        [int]$TimeoutMs
    )

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    $lines = New-Object System.Collections.Generic.List[string]

    while ((Get-Date) -lt $deadline) {
        try {
            $line = $Port.ReadLine().Trim()
            if ($line.Length -gt 0) {
                $lines.Add($line)
                Write-Host $line
                if (& $StopWhen $line) {
                    break
                }
            }
        }
        catch [TimeoutException] {
        }
    }

    return $lines
}

function Send-ConsoleCommand {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [string]$Command,
        [ScriptBlock]$StopWhen,
        [int]$TimeoutMs
    )

    Write-Host "> $Command"
    $Port.Write("`n")
    Start-Sleep -Milliseconds 50
    $Port.Write("$Command`n")
    return Read-LinesUntil -Port $Port -StopWhen $StopWhen -TimeoutMs $TimeoutMs
}

function Wait-ConsoleReady {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int]$TimeoutMs
    )

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $statusLines = Send-ConsoleCommand -Port $Port -Command "status" -TimeoutMs 4000 -StopWhen {
            param($line)
            $line -like 'STATUS *'
        }

        $statusLine = $statusLines | Where-Object { $_ -like 'STATUS *' } | Select-Object -Last 1
        if ($statusLine -and $statusLine -match 'state=Ready' -and $statusLine -match 'schema=yes') {
            return $statusLine
        }

        Start-Sleep -Milliseconds 500
    }

    throw "Task did not reach Ready/schema=yes within ${TimeoutMs}ms."
}

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.NewLine = "`n"
$port.ReadTimeout = 250
$port.WriteTimeout = 1000

try {
    $port.Open()
}
catch {
    throw "Failed to open ${PortName}: $($_.Exception.Message)"
}

try {
    Start-Sleep -Milliseconds 500
    Read-LinesUntil -Port $port -TimeoutMs $OpenTimeoutMs -StopWhen { param($line) $false } | Out-Null

    if ($EnableTrace) {
        Send-ConsoleCommand -Port $port -Command "trace on" -TimeoutMs 4000 -StopWhen { param($line) $line -eq 'TRACE on' } | Out-Null
    }

    Wait-ConsoleReady -Port $port -TimeoutMs $OpenTimeoutMs | Out-Null

    if ([string]::IsNullOrWhiteSpace($ParameterName)) {
        $nameLines = Send-ConsoleCommand -Port $port -Command "names 1" -TimeoutMs 4000 -StopWhen { param($line) $line -like 'NAME[[]0[]]*' -or $line -like 'NAMES *' }
        $nameLine = $nameLines | Where-Object { $_ -like 'NAME[[]0[]]*' } | Select-Object -First 1
        if (-not $nameLine) {
            throw "Could not discover a parameter name from schema."
        }

        if ($nameLine -match '^NAME\[0\]\s+([^\s]+)\s+id=') {
            $ParameterName = $matches[1]
        }
        else {
            throw "Unexpected NAME line format: $nameLine"
        }
    }

    Write-Host "Using parameter: $ParameterName"
    $targetLines = Send-ConsoleCommand -Port $port -Command "taskget $ParameterName" -TimeoutMs $CommandTimeoutMs -StopWhen { param($line) $line -like 'TARGET *' }
    $targetLine = $targetLines | Where-Object { $_ -like 'TARGET *' } | Select-Object -Last 1
    $expectedTarget = 'TARGET submitted=1 accepted=1 values=1 completed=1 failed=0 timeouts=0 busy=0 result=PASS'

    if (-not $targetLine) {
        throw "No TARGET summary line received."
    }

    if ($targetLine -ne $expectedTarget) {
        throw "Unexpected target result. Expected: $expectedTarget Got: $targetLine"
    }

    Write-Host "Console test passed."
}
finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
