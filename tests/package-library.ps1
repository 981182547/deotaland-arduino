param([string]$OutputPath)
$ErrorActionPreference='Stop'
$libraryRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workspace=Split-Path -Parent $libraryRoot
$version=((Get-Content -LiteralPath (Join-Path $libraryRoot 'library.properties') | Where-Object { $_ -match '^version=' }) -replace '^version=','')
if(-not $OutputPath) { $OutputPath=Join-Path $workspace "dist/deotaland-arduino-$version.zip" }
$stage=Join-Path $workspace ('.build/package-'+[guid]::NewGuid().ToString('N'))
$dest=Join-Path $stage 'deotaland-arduino'
New-Item -ItemType Directory -Force -Path $dest,(Split-Path -Parent $OutputPath) | Out-Null
foreach($name in @('src','examples','docs')) {
    $source=Join-Path $libraryRoot $name
    foreach($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
        $relative=$file.FullName.Substring($libraryRoot.Length+1)
        if($relative -match '(^|[\\/])(build|\.build)([\\/]|$)') { continue }
        $target=Join-Path $dest $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}
foreach($name in @('library.properties','keywords.txt','LICENSE','README.md')) {
    Copy-Item -LiteralPath (Join-Path $libraryRoot $name) -Destination $dest
}
# Include validation records, but exclude compiler products and test sources.
New-Item -ItemType Directory -Path (Join-Path $dest 'tests') | Out-Null
foreach($file in Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object {
    $_.Name -match '^(VALIDATION.*\.md|BUILD.*\.json|SOURCE-SHA256.*\.json|README\.md)$'
}) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $dest 'tests')
}
Compress-Archive -LiteralPath $dest -DestinationPath $OutputPath -Force
Get-Item -LiteralPath $OutputPath | Select-Object FullName,Length
