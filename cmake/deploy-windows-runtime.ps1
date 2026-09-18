<#
.SYNOPSIS
    Deploy the runtime DLLs, OSG plugins and Qt libraries next to the built OpenMW
    executables so the build directory is directly runnable (without an install step).

.DESCRIPTION
    OpenMW on Windows is normally run from a packaged install; the raw build directory
    is missing several runtime pieces the loader needs:
      * Qt6 (Qt lives outside vcpkg, so vcpkg's applocal step never copies it)
      * MyGUIEngine.dll (vcpkg ships it under bin/Release, which applocal doesn't scan)
      * OSG plugins (osgPlugins-<ver>/osgdb_*.dll are loaded at runtime, not imported,
        so nothing copies them automatically)
      * the vcpkg DLL closure that applocal missed (e.g. MyGUIEngine's freetype/brotli/png)
      * bundled DLLs built in-tree (e.g. yaml-cpp)

    All paths are read from the build directory's CMakeCache.txt, so nothing is hardcoded.
    The script is best-effort and idempotent: it only copies files that are missing or
    newer, and always exits 0 so it can be used as a POST_BUILD step without ever
    breaking the build.

.PARAMETER BuildDir
    The CMake build directory (contains CMakeCache.txt and the built .exe files).
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Continue'

function Get-CacheValue([string]$cacheText, [string]$name) {
    # Matches lines like  NAME:TYPE=value
    $m = [regex]::Match($cacheText, "(?m)^\s*$([regex]::Escape($name)):[^=]*=(.*)$")
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return $null
}

function Copy-IfNewer([string]$src, [string]$dstDir) {
    try {
        if (-not (Test-Path -LiteralPath $src)) { return }
        $name = Split-Path -Leaf $src
        $dst = Join-Path $dstDir $name
        if ((Test-Path -LiteralPath $dst) -and
            ((Get-Item -LiteralPath $dst).LastWriteTimeUtc -ge (Get-Item -LiteralPath $src).LastWriteTimeUtc)) {
            return
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
    } catch { Write-Host "deploy: skip $src ($_)" }
}

try {
    if (-not (Test-Path -LiteralPath $BuildDir)) { Write-Host "deploy: build dir not found: $BuildDir"; exit 0 }
    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cacheFile)) { Write-Host "deploy: no CMakeCache.txt in $BuildDir"; exit 0 }
    $cache = Get-Content -LiteralPath $cacheFile -Raw

    $vcpkgInstalled = Get-CacheValue $cache '_VCPKG_INSTALLED_DIR'
    $triplet        = Get-CacheValue $cache 'VCPKG_TARGET_TRIPLET'
    $qtPrefix       = Get-CacheValue $cache 'CMAKE_PREFIX_PATH'
    if (-not $qtPrefix) { $qt6dir = Get-CacheValue $cache 'Qt6_DIR'; if ($qt6dir) { $qtPrefix = (Resolve-Path (Join-Path $qt6dir '..\..\..')).Path } }
    $osgVer         = Get-CacheValue $cache 'OPENSCENEGRAPH_VERSION'

    # ---- vcpkg runtime DLL closure (release) ----
    if ($vcpkgInstalled -and $triplet) {
        $vcpkgBin = Join-Path $vcpkgInstalled (Join-Path $triplet 'bin')
        if (Test-Path -LiteralPath $vcpkgBin) {
            Get-ChildItem -LiteralPath $vcpkgBin -Filter *.dll -File -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-IfNewer $_.FullName $BuildDir }
            # MyGUI ships its DLL under bin/Release
            $rel = Join-Path $vcpkgBin 'Release'
            if (Test-Path -LiteralPath $rel) {
                Get-ChildItem -LiteralPath $rel -Filter *.dll -File -ErrorAction SilentlyContinue |
                    ForEach-Object { Copy-IfNewer $_.FullName $BuildDir }
            }
            # OSG runtime plugins (loaded dynamically, not imported)
            if ($osgVer) {
                $plugSrc = Join-Path $vcpkgBin "osgPlugins-$osgVer"
                if (Test-Path -LiteralPath $plugSrc) {
                    $plugDst = Join-Path $BuildDir "osgPlugins-$osgVer"
                    if (-not (Test-Path -LiteralPath $plugDst)) { New-Item -ItemType Directory -Path $plugDst | Out-Null }
                    Get-ChildItem -LiteralPath $plugSrc -Filter *.dll -File -ErrorAction SilentlyContinue |
                        ForEach-Object { Copy-IfNewer $_.FullName $plugDst }
                }
            }
        } else { Write-Host "deploy: vcpkg bin not found: $vcpkgBin" }
    }

    # ---- bundled in-tree DLLs (e.g. yaml-cpp built under _deps) ----
    $depsDir = Join-Path $BuildDir '_deps'
    if (Test-Path -LiteralPath $depsDir) {
        Get-ChildItem -LiteralPath $depsDir -Filter *.dll -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -notmatch '\\osgPlugins-' } |
            ForEach-Object { Copy-IfNewer $_.FullName $BuildDir }
    }

    # ---- Qt runtime + plugins via windeployqt ----
    if ($qtPrefix) {
        $windeployqt = Join-Path $qtPrefix 'bin\windeployqt.exe'
        if (Test-Path -LiteralPath $windeployqt) {
            $env:PATH = (Join-Path $qtPrefix 'bin') + ';' + $env:PATH
            foreach ($exe in @('openmw-launcher.exe', 'openmw-wizard.exe', 'openmw-cs.exe')) {
                $exePath = Join-Path $BuildDir $exe
                if (Test-Path -LiteralPath $exePath) {
                    & $windeployqt --release --no-translations --no-compiler-runtime "$exePath" | Out-Null
                }
            }
        } else { Write-Host "deploy: windeployqt not found under $qtPrefix" }
    }

    Write-Host "deploy: runtime deployment complete for $BuildDir"
} catch {
    Write-Host "deploy: non-fatal error: $_"
}

exit 0
