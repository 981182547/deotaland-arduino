param(
    [string]$Fqbn = 'esp32:esp32:esp32s3:PartitionScheme=min_spiffs',
    [string]$BuildRoot
)
$ErrorActionPreference = 'Stop'
$libraryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $libraryRoot '.build\arduino-matrix' }
$buildPath = Join-Path $BuildRoot 'build'
$logPath = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $buildPath, $logPath | Out-Null
$results = @()
foreach ($example in (Get-ChildItem -LiteralPath (Join-Path $libraryRoot 'examples') -Directory | Sort-Object Name)) {
    $log = Join-Path $logPath ($example.Name + '.log')
    $ErrorActionPreference = 'Continue'
    $extraArgs = @()
    $sketchPath = $example.FullName
    if ($example.Name -eq 'BasicWiFi') {
        # Exercise the configured branch without putting real credentials in logs.
        $sketchPath = Join-Path $BuildRoot 'configured/BasicWiFi'
        New-Item -ItemType Directory -Force -Path $sketchPath | Out-Null
        $text = [IO.File]::ReadAllText((Join-Path $example.FullName 'BasicWiFi.ino'))
        $text = $text.Replace('const char kSsid[] = "YOUR_WIFI_SSID";', 'const char kSsid[] = "compile-only-network";')
        $text = $text.Replace('const char kPassword[] = "YOUR_WIFI_PASSWORD";', 'const char kPassword[] = "compile-only-password";')
        [IO.File]::WriteAllText((Join-Path $sketchPath 'BasicWiFi.ino'), $text, [Text.UTF8Encoding]::new($false))
    }
    if ($example.Name -eq 'CloudActivation') {
        $extraArgs = @('--build-property','compiler.cpp.extra_flags=-DDEOTALAND_CLOUD_PRODUCT_ID=1')
    }
    & arduino-cli compile --fqbn $Fqbn --library $libraryRoot --build-path $buildPath --warnings all @extraArgs $sketchPath *> $log
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $output = Get-Content -LiteralPath $log -Raw
    $flash = if ($output -match 'Sketch uses (\d+) bytes') { [int]$matches[1] } else { $null }
    $ram = if ($output -match 'Global variables use (\d+) bytes') { [int]$matches[1] } else { $null }
    $warnings = ([regex]::Matches($output, 'warning:')).Count
    $result = [pscustomobject]@{
        Example = $example.Name; Fqbn = $Fqbn; ExitCode = $exitCode
        FlashBytes = $flash; StaticRamBytes = $ram; Warnings = $warnings; Log = $log
    }
    $results += $result
    Write-Output ("{0}: exit={1}, flash={2}, RAM={3}, warnings={4}" -f $result.Example,$exitCode,$flash,$ram,$warnings)
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logPath 'results.json') -Encoding UTF8
if ($results | Where-Object { $_.ExitCode -ne 0 }) { throw "Arduino matrix failed; inspect $logPath" }
