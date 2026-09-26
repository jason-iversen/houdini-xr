# No param() block on purpose: every argument here is forwarded to hxr.exe, and
# a declared parameter would swallow leading "--flag" tokens before $args sees them.
# Overrides come from the environment instead: HXR_HFS, HXR_CONFIG.

# Newest install by default; see build.ps1 for why this isn't pinned.
$hfs = if ($env:HXR_HFS) { $env:HXR_HFS }
       else {
           Get-ChildItem "C:\Program Files\Side Effects Software" -Directory -ErrorAction Ignore |
               Where-Object { $_.Name -match '^Houdini \d+\.\d+\.\d+$' } |
               Sort-Object { [version]($_.Name -replace '^Houdini ', '') } |
               Select-Object -Last 1 -ExpandProperty FullName
       }
if (-not $hfs) { throw "No Houdini install found; set HXR_HFS" }

$config = if ($env:HXR_CONFIG) { $env:HXR_CONFIG } else { "Release" }

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\$config\hxr.exe"

if (-not (Test-Path $exe)) { throw "Not built yet: $exe  (run scripts\build.ps1)" }

# Houdini-native delegates (Karma especially) expect Houdini's own environment
# -- HOUDINI_PATH for VEX shader resolution and so on. hconfig prints exactly
# what houdini.exe would run with; import it, minus PATH which is managed
# below. HFS is then re-set to the long-form path since hconfig reports the
# 8.3 short form, and hxr.exe uses HFS to fix the DLL search order.
& "$hfs\bin\hconfig.exe" | ForEach-Object {
    if ($_ -match "^(\w+) := '(.*)'$" -and $Matches[1] -ne 'PATH' -and $Matches[2] -ne '<not defined>') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
    }
}
$env:HFS = $hfs

# The libpxr_*.dll files live in Houdini's bin, and render delegates are
# discovered through plugInfo.json files. Storm's is under bin\usd_plugins;
# Houdini's own delegates (Karma CPU/XPU, Houdini VK) register separately
# under houdini\dso\usd_plugins. Without both paths the process either fails
# to load or only ever sees Storm.
$env:PATH = "$hfs\bin;$env:PATH"
$env:PXR_PLUGINPATH_NAME = "$hfs\bin\usd_plugins;$hfs\houdini\dso\usd_plugins"

# Storm is GL-only here, so the runtime must expose XR_KHR_opengl_enable. The
# Oculus PC runtime does (confirmed with --probe), so the system default is left
# alone. Set HXR_XR_RUNTIME to a runtime manifest to override for this process
# only; that does not change the system-wide active runtime.
if ($env:HXR_XR_RUNTIME) {
    if (Test-Path $env:HXR_XR_RUNTIME) {
        $env:XR_RUNTIME_JSON = $env:HXR_XR_RUNTIME
    } else {
        Write-Warning "OpenXR runtime manifest not found: $($env:HXR_XR_RUNTIME)"
    }
}

& $exe @args
exit $LASTEXITCODE
