$ErrorActionPreference = 'Stop'
$libraryRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot '.build/playback'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ Build Tools.' }
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
    & cl.exe /nologo /std:c++14 /EHsc /W4 /utf-8 "/I$(Join-Path $libraryRoot 'src')" (Join-Path $libraryRoot 'src/audio_downlink.cpp') (Join-Path $PSScriptRoot 'playback_tests.cpp') /Fe:playback_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Playback test compilation failed.' }
    & (Join-Path $buildDir 'playback_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Playback tests failed.' }
} finally {
    Pop-Location
}
