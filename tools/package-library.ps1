param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
$libraryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workspace = Split-Path -Parent $libraryRoot
$properties = [IO.File]::ReadAllText((Join-Path $libraryRoot 'library.properties'))
$version = [regex]::Match($properties, '(?m)^version=(.+)$').Groups[1].Value.Trim()
if (-not $version) { throw 'Missing library version' }
if (-not $OutputPath) {
    $OutputPath = Join-Path $workspace "dist/deotaland-arduino-$version.zip"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$stage = Join-Path ([IO.Path]::GetTempPath()) ('deotaland-package-' + [guid]::NewGuid().ToString('N'))
$dest = Join-Path $stage 'deotaland-arduino'
New-Item -ItemType Directory -Path $dest -Force | Out-Null
try {
    foreach ($name in @('src', 'examples', 'docs')) {
        $source = Join-Path $libraryRoot $name
        foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
            $relative = $file.FullName.Substring($libraryRoot.Length + 1)
            if ($relative -match '(^|[\\/])(build|\.build(?:[-_][^\\/]*)?|managed_components|__pycache__)([\\/]|$)') { continue }
            if ($relative -match '\.(bin|elf|map|o|obj|d|pyc|log|tmp|zip)$') { continue }
            if ($file.Name -match '^(sdkconfig(?:\.old)?|dependencies\.lock)$') { continue }
            $target = Join-Path $dest $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }
    foreach ($name in @('library.properties', 'keywords.txt', 'LICENSE', 'README.md')) {
        Copy-Item -LiteralPath (Join-Path $libraryRoot $name) -Destination $dest
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
    Compress-Archive -LiteralPath $dest -DestinationPath $OutputPath -Force
    Get-Item -LiteralPath $OutputPath | Select-Object FullName, Length
} finally {
    # Only remove this invocation's staging directory, after checking its path.
    if (Test-Path -LiteralPath $stage) {
        $resolved = (Resolve-Path -LiteralPath $stage).Path
        if (-not $resolved.Equals([IO.Path]::GetFullPath($stage), [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected staging path: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse
    }
}
