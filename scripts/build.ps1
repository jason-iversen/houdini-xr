param(
    [string]$Hfs    = "C:\Program Files\Side Effects Software\Houdini 22.0.432",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$hfsFwd = $Hfs -replace '\\', '/'

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 "-DHFS=$hfsFwd"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "Built: $build\$Config\hxr.exe"
