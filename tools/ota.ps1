param(
    [ValidateSet("Initialize", "Build", "Upload")][string]$Action = "Upload",
    [string]$Address = "codexmeter-c6.local",
    [string]$Firmware,
    [string]$ArduinoCli = "arduino-cli",
    [string]$Python = "python",
    [string]$Libraries,
    [string]$BuildPath,
    [string]$Espota,
    [int]$HostPort = 3233
)
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$credentialPath = Join-Path $root ".local/ota-password.dpapi"
$headerPath = Join-Path $root "src/ota.local.h"
if (-not $BuildPath) { $BuildPath = Join-Path $root ".arduino-build-ota" }
if (-not $Firmware) { $Firmware = Join-Path $BuildPath ((Split-Path $root -Leaf) + ".ino.bin") }

function Read-OtaPassword {
    if ($env:CODEX_C6_OTA_PASSWORD) { return $env:CODEX_C6_OTA_PASSWORD }
    if (-not (Test-Path -LiteralPath $credentialPath)) {
        throw "Initialize OTA credentials first, or set CODEX_C6_OTA_PASSWORD locally."
    }
    $secure = (Get-Content -LiteralPath $credentialPath -Raw).Trim() | ConvertTo-SecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

if ($Action -eq "Initialize") {
    if (-not [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([Runtime.InteropServices.OSPlatform]::Windows)) {
        throw "DPAPI initialization requires Windows."
    }
    if (Test-Path -LiteralPath $credentialPath) {
        Write-Host "Existing DPAPI credential preserved."
        exit 0
    }
    $bytes = [byte[]]::new(32)
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $secure = ConvertTo-SecureString ([Convert]::ToBase64String($bytes)) -AsPlainText -Force
    New-Item -ItemType Directory -Path (Split-Path $credentialPath) -Force | Out-Null
    $secure | ConvertFrom-SecureString | Set-Content -LiteralPath $credentialPath -Encoding ascii
    [Array]::Clear($bytes, 0, $bytes.Length)
    Write-Host "Random OTA credential saved with Windows DPAPI for the current user."
    exit 0
}

$password = Read-OtaPassword
if ($password.Length -lt 16) { throw "OTA password must contain at least 16 characters." }
if ($Action -eq "Build") {
    if (Test-Path -LiteralPath $headerPath) {
        throw "src/ota.local.h already exists; refusing to overwrite a local credential header."
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($password))).ToLowerInvariant() }
    finally { $sha.Dispose() }
    $password = $null
    try {
        [IO.File]::WriteAllText($headerPath, "#pragma once`nconstexpr char kOtaPasswordHash[] = `"$hash`";`n")
        $compileArgs = @("compile", "--fqbn", "esp32:esp32:esp32c6:FlashSize=16M",
                         "--build-path", $BuildPath)
        if ($Libraries) { $compileArgs += @("--libraries", $Libraries) }
        & $ArduinoCli @compileArgs $root
        if ($LASTEXITCODE -ne 0) { throw "Arduino compilation failed." }
    }
    finally {
        # Only remove the exact temporary file created above, never a directory.
        Remove-Item -LiteralPath $headerPath -ErrorAction SilentlyContinue
        $hash = $null
    }
    exit 0
}

if (-not $Espota) {
    # Match the documented/pinned core. Older uploaders use incompatible auth.
    $Espota = Join-Path $env:LOCALAPPDATA "Arduino15/packages/esp32/hardware/esp32/3.3.11/tools/espota.py"
}
if (-not (Test-Path -LiteralPath $Espota)) { throw "Pass -Espota with the uploader from Arduino-ESP32 3.3.11." }
if (-not (Test-Path -LiteralPath $Firmware)) { throw "Firmware file does not exist; build first." }
$previousPassword = $env:CODEX_C6_OTA_PASSWORD
try {
    # Only pass the password through the child environment, never argv or logs.
    $env:CODEX_C6_OTA_PASSWORD = $password
    & $Python (Join-Path $PSScriptRoot "upload_ota.py") --address $Address --firmware $Firmware --espota $Espota --host-port $HostPort
    if ($LASTEXITCODE -ne 0) { throw "OTA upload failed; the running image is retained unless activation completed." }
}
finally {
    $env:CODEX_C6_OTA_PASSWORD = $previousPassword
    $password = $null
}
