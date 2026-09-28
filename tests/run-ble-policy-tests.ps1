$ErrorActionPreference = 'Stop'
$libraryRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot '.build/ble-policy'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ Build Tools were not found.' }
    $devCmd = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $setupCommand = '"{0}" -arch=x64 -host_arch=x64 >nul && set' -f $devCmd
    $environment = & cmd.exe /d /s /c $setupCommand
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio compiler environment.' }
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}
Push-Location $buildDir
try {
    & cl.exe /nologo /std:c++14 /EHsc /W4 /utf-8 "/I$(Join-Path $libraryRoot 'src')" (Join-Path $PSScriptRoot 'ble_policy_tests.cpp') /Fe:ble_policy_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'BLE policy test compilation failed.' }
    & (Join-Path $buildDir 'ble_policy_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'BLE policy tests failed.' }
} finally { Pop-Location }
