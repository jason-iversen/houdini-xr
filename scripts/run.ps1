# No param() block on purpose: every argument here is forwarded to hxr.exe, and
# a declared parameter would swallow leading "--flag" tokens before $args sees them.
# Overrides come from the environment instead: HXR_HFS, HXR_CONFIG.

$hfs = if ($env:HXR_HFS) { $env:HXR_HFS }
       else { "C:\Program Files\Side Effects Software\Houdini 22.0.432" }

$config = if ($env:HXR_CONFIG) { $env:HXR_CONFIG } else { "Release" }

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\$config\hxr.exe"

if (-not (Test-Path $exe)) { throw "Not built yet: $exe  (run scripts\build.ps1)" }

# The libpxr_*.dll files live in Houdini's bin, and Storm's render delegate is
# discovered through plugInfo.json under bin\usd_plugins. Without both, the
# process either fails to load or comes up with no renderer.
$env:PATH = "$hfs\bin;$env:PATH"
$env:PXR_PLUGINPATH_NAME = "$hfs\bin\usd_plugins"

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
