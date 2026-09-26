param(
    [string]$Hfs    = "",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

# Newest install by default -- Houdini point releases delete the old directory,
# and the previous HFS is cached in CMakeCache.txt, so pinning a version here
# turns a routine update into a confusing configure failure. Passing -DHFS on
# every configure (below) also refreshes that stale cache entry.
if (-not $Hfs) {
    $Hfs = Get-ChildItem "C:\Program Files\Side Effects Software" -Directory -ErrorAction Ignore |
        Where-Object { $_.Name -match '^Houdini \d+\.\d+\.\d+$' } |
        Sort-Object { [version]($_.Name -replace '^Houdini ', '') } |
        Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $Hfs) { throw "No Houdini install found; pass -Hfs <path>" }
Write-Host "HFS: $Hfs"

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$hfsFwd = $Hfs -replace '\\', '/'

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 "-DHFS=$hfsFwd"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "Built: $build\$Config\hxr.exe"
