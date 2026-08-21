param(
    [string]$BrokerHost = $env:CODEX_C6_MQTT_HOST,
    [int]$BrokerPort = 1883,
    [string]$Username = $env:CODEX_C6_MQTT_USERNAME,
    [string]$Password = $env:CODEX_C6_MQTT_PASSWORD,
    [string]$Topic = $(if ($env:CODEX_C6_MQTT_TOPIC) { $env:CODEX_C6_MQTT_TOPIC } else { "codex/usage/c6/state" }),
    [string]$ReferenceScript = $env:CODEX_USAGE_EXPORTER_SCRIPT,
    [int]$RefreshSeconds = 1800,
    [switch]$NoRetain,
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ReferenceScript)) {
    throw "Set CODEX_USAGE_EXPORTER_SCRIPT or pass -ReferenceScript."
}
if (-not $DryRun -and [string]::IsNullOrWhiteSpace($BrokerHost)) {
    throw "Set CODEX_C6_MQTT_HOST or pass -BrokerHost."
}

function New-MqttUtf8String {
    param([string]$Value)

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
    if ($bytes.Length -gt 65535) {
        throw "MQTT UTF-8 string is too long: $($bytes.Length) bytes"
    }
    return [byte[]]@( (($bytes.Length -shr 8) -band 0xFF), ($bytes.Length -band 0xFF) ) + $bytes
}

function New-MqttRemainingLength {
    param([int]$Length)

    $encoded = New-Object System.Collections.Generic.List[byte]
    do {
        $digit = $Length % 128
        $Length = [Math]::Floor($Length / 128)
        if ($Length -gt 0) {
            $digit = $digit -bor 0x80
        }
        $encoded.Add([byte]$digit)
    } while ($Length -gt 0)
    return $encoded.ToArray()
}

function Read-MqttExact {
    param(
        [System.IO.Stream]$Stream,
        [byte[]]$Buffer,
        [int]$Offset,
        [int]$Count
    )

    $readTotal = 0
    while ($readTotal -lt $Count) {
        $read = $Stream.Read($Buffer, $Offset + $readTotal, $Count - $readTotal)
        if ($read -le 0) {
            throw "MQTT broker closed the connection while reading"
        }
        $readTotal += $read
    }
}

function Invoke-MqttPublish {
    param(
        [string]$HostName,
        [int]$Port,
        [string]$User,
        [string]$Pass,
        [string]$PublishTopic,
        [string]$Message,
        [bool]$Retain
    )

    $clientId = "codex-c6-$([Guid]::NewGuid().ToString('N').Substring(0, 12))"
    $connectFlags = 0x02
    if ($User) { $connectFlags = $connectFlags -bor 0x80 }
    if ($Pass) { $connectFlags = $connectFlags -bor 0x40 }

    $variableHeader = (New-MqttUtf8String "MQTT") + [byte[]]@(0x04, $connectFlags, 0x00, 0x3C)
    $connectPayload = New-MqttUtf8String $clientId
    if ($User) { $connectPayload += New-MqttUtf8String $User }
    if ($Pass) { $connectPayload += New-MqttUtf8String $Pass }
    $connectPacketPayload = $variableHeader + $connectPayload
    $connectPacket = [byte[]]@(0x10) + (New-MqttRemainingLength $connectPacketPayload.Length) + $connectPacketPayload

    $topicBytes = New-MqttUtf8String $PublishTopic
    $messageBytes = [System.Text.Encoding]::UTF8.GetBytes($Message)
    $publishType = if ($Retain) { [byte]0x31 } else { [byte]0x30 }
    $publishPayload = $topicBytes + $messageBytes
    $publishPacket = [byte[]]@($publishType) + (New-MqttRemainingLength $publishPayload.Length) + $publishPayload
    $disconnectPacket = [byte[]]@(0xE0, 0x00)

    $tcpClient = [System.Net.Sockets.TcpClient]::new()
    try {
        $tcpClient.Connect($HostName, $Port)
        $stream = $tcpClient.GetStream()
        $stream.Write($connectPacket, 0, $connectPacket.Length)

        $connack = [byte[]]::new(4)
        Read-MqttExact -Stream $stream -Buffer $connack -Offset 0 -Count $connack.Length
        if ($connack[0] -ne 0x20 -or $connack[1] -ne 0x02 -or $connack[3] -ne 0x00) {
            throw "MQTT broker rejected connection. CONNACK: $($connack -join ',')"
        }

        $stream.Write($publishPacket, 0, $publishPacket.Length)
        $stream.Write($disconnectPacket, 0, $disconnectPacket.Length)
    }
    finally {
        $tcpClient.Close()
    }
}

function Get-PropertyValue {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-NumberValue {
    param(
        [object]$Object,
        [string]$Name
    )

    $value = Get-PropertyValue -Object $Object -Name $Name
    if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
        return $null
    }
    try {
        return [double]$value
    }
    catch {
        return $null
    }
}

function Get-ExporterState {
    if (-not (Test-Path -LiteralPath $ReferenceScript)) {
        throw "Reference exporter script not found: $ReferenceScript"
    }

    $output = @(& $ReferenceScript -DryRun)
    if ($LASTEXITCODE -ne 0) {
        throw "Reference exporter dry-run failed"
    }

    for ($index = $output.Count - 1; $index -ge 0; --$index) {
        try {
            $candidate = [string]$output[$index] | ConvertFrom-Json
            if ($candidate -is [pscustomobject]) {
                return $candidate
            }
        }
        catch {
            continue
        }
    }
    throw "Reference exporter dry-run returned no JSON object"
}

function Clamp-Percent {
    param([double]$Value)
    return [int][Math]::Max(0, [Math]::Min(100, [Math]::Round($Value)))
}

if ($RefreshSeconds -lt 1) {
    throw "RefreshSeconds must be positive"
}

$state = Get-ExporterState
$weeklyPrefix = $null
foreach ($prefix in @("codex_weekly", "codex_5h")) {
    $window = Get-NumberValue -Object $state -Name "${prefix}_window_mins"
    $used = Get-NumberValue -Object $state -Name "${prefix}_used_percent"
    $remaining = Get-NumberValue -Object $state -Name "${prefix}_remaining_percent"
    if ($null -ne $window -and [int][Math]::Round($window) -eq 10080 -and
        ($null -ne $used -or $null -ne $remaining)) {
        $weeklyPrefix = $prefix
        break
    }
}

if ($null -eq $weeklyPrefix) {
    throw "Reference exporter returned no 7-day quota window"
}

$usedValue = Get-NumberValue -Object $state -Name "${weeklyPrefix}_used_percent"
$remainingValue = Get-NumberValue -Object $state -Name "${weeklyPrefix}_remaining_percent"
if ($null -eq $remainingValue -and $null -ne $usedValue) {
    $remainingValue = 100 - $usedValue
}
if ($null -eq $remainingValue) {
    throw "Reference exporter returned no weekly remaining percentage"
}

$remaining = Clamp-Percent $remainingValue
$used = if ($null -ne $usedValue) { Clamp-Percent $usedValue } else { 100 - $remaining }
$windowMins = 10080
$resetAt = Get-NumberValue -Object $state -Name "${weeklyPrefix}_reset_at_epoch"
$capturedAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$resetsIn = if ($null -ne $resetAt) {
    [int][Math]::Max(0, [Math]::Round($resetAt - $capturedAt))
} else {
    -1
}

$plan = [string](Get-PropertyValue -Object $state -Name "codex_plan_type")
if ([string]::IsNullOrWhiteSpace($plan)) { $plan = "CODEX" }
$model = [string](Get-PropertyValue -Object $state -Name "codex_model")
if ([string]::IsNullOrWhiteSpace($model)) { $model = "MODEL UNKNOWN" }

$payload = [ordered]@{
    v = 1
    status = "ok"
    source = "MQTT EXPORTER"
    capturedAt = $capturedAt
    nextPollIn = $RefreshSeconds
    plan = $plan.ToUpperInvariant()
    planLabel = $plan.ToUpperInvariant()
    modelLabel = $model.ToUpperInvariant()
    preferred = [ordered]@{
        primary = [ordered]@{
            used = $used
            remaining = $remaining
            windowMins = $windowMins
            resetsIn = $resetsIn
        }
    }
}

$json = $payload | ConvertTo-Json -Compress -Depth 6
if ($DryRun) {
    $json
    exit 0
}

Invoke-MqttPublish `
    -HostName $BrokerHost `
    -Port $BrokerPort `
    -User $Username `
    -Pass $Password `
    -PublishTopic $Topic `
    -Message $json `
    -Retain (-not $NoRetain)

Write-Host "Published compact Codex usage to mqtt://$BrokerHost`:$BrokerPort/$Topic retained=$(-not $NoRetain) bytes=$([System.Text.Encoding]::UTF8.GetByteCount($json))"
