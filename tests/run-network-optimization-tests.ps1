$ErrorActionPreference = 'Stop'
$libraryRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot '.build-network-optimization'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ Build Tools were not found.' }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
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
    & cl.exe /nologo /std:c++14 /EHsc /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$(Join-Path $libraryRoot 'src')" (Join-Path $libraryRoot 'src/cloud_session.cpp') (Join-Path $libraryRoot 'src/cloud_status.cpp') (Join-Path $PSScriptRoot 'network_optimization_tests.cpp') /Fe:network_optimization_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Network optimization test compilation failed.' }
    & (Join-Path $buildDir 'network_optimization_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Network optimization tests failed.' }
    & cl.exe /nologo /std:c++14 /EHsc /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$(Join-Path $libraryRoot 'src')" "/I$(Join-Path $PSScriptRoot 'network_freertos')" (Join-Path $PSScriptRoot 'worker_wake_tests.cpp') /Fe:worker_wake_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Worker wake test compilation failed.' }
    & (Join-Path $buildDir 'worker_wake_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Worker wake tests failed.' }
} finally {
    Pop-Location
}
