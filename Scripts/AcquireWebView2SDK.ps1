# AcquireWebView2SDK.ps1
# Downloads Microsoft.Web.WebView2 from NuGet and extracts the pieces
# that InoWebUI.Build.cs expects into Source/ThirdParty/WebView2/.
#
# Usage:
#   .\Scripts\AcquireWebView2SDK.ps1              # auto-detect latest stable
#   .\Scripts\AcquireWebView2SDK.ps1 -Version 1.0.2849.39
#
# Run from the InoWebUI plugin root (or anywhere — the script finds its own location).

param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$PackageName = "Microsoft.Web.WebView2"

# Resolve paths relative to this script's location
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $ScriptDir
$DestDir    = Join-Path $PluginRoot "Source" "ThirdParty" "WebView2"

# ── 1. Resolve version ────────────────────────────────────────────────────────
if (-not $Version) {
    Write-Host "Querying NuGet for latest stable $PackageName..." -ForegroundColor Cyan
    $indexJson = Invoke-RestMethod `
        "https://api.nuget.org/v3-flatcontainer/$($PackageName.ToLower())/index.json"

    # Stable versions look like 1.0.NNNN.NN  (four numeric components, no suffix)
    $Version = $indexJson.versions |
        Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } |
        Select-Object -Last 1

    if (-not $Version) {
        throw "Could not determine latest stable version. Pass -Version explicitly."
    }
    Write-Host "  Latest stable: $Version" -ForegroundColor Cyan
}

# Check if already installed at this version
$VersionFile = Join-Path $DestDir "VERSION"
if ((Test-Path $VersionFile) -and ((Get-Content $VersionFile).Trim() -eq $Version)) {
    Write-Host "WebView2 SDK $Version is already installed. Nothing to do." -ForegroundColor Green
    exit 0
}

# ── 2. Download .nupkg (it's just a zip) ─────────────────────────────────────
$TempFile    = Join-Path $env:TEMP "WebView2.$Version.nupkg"
$TempExtract = Join-Path $env:TEMP "WebView2.$Version"

if (-not (Test-Path $TempFile)) {
    $Url = "https://www.nuget.org/api/v2/package/$PackageName/$Version"
    Write-Host "Downloading $Url ..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $Url -OutFile $TempFile
} else {
    Write-Host "Using cached download: $TempFile" -ForegroundColor DarkGray
}

# ── 3. Extract ────────────────────────────────────────────────────────────────
if (Test-Path $TempExtract) { Remove-Item $TempExtract -Recurse -Force }
New-Item -ItemType Directory -Path $TempExtract | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($TempFile, $TempExtract)

# ── 4. Copy into plugin ThirdParty tree ──────────────────────────────────────
$IncDst = Join-Path $DestDir "include"
$LibDst = Join-Path $DestDir "lib" "Win64"

New-Item -ItemType Directory -Force -Path $IncDst | Out-Null
New-Item -ItemType Directory -Force -Path $LibDst | Out-Null

# Headers
foreach ($h in @("WebView2.h", "WebView2EnvironmentOptions.h")) {
    $src = Join-Path $TempExtract "build" "native" "include" $h
    if (Test-Path $src) {
        Copy-Item $src $IncDst -Force
    } else {
        Write-Warning "Header not found in package: $h"
    }
}

# Static loader lib (links into our binary — no extra DLL to ship at runtime)
$StaticLib = Join-Path $TempExtract "build" "native" "x64" "WebView2LoaderStatic.lib"
if (Test-Path $StaticLib) {
    Copy-Item $StaticLib $LibDst -Force
} else {
    throw "WebView2LoaderStatic.lib not found in package. Package structure may have changed."
}

# Runtime DLL (optional — only needed if you want to redistribute a fixed version)
$RuntimeDll = Join-Path $TempExtract "runtimes" "win-x64" "native" "WebView2Loader.dll"
if (Test-Path $RuntimeDll) {
    Copy-Item $RuntimeDll $LibDst -Force
}

# Stamp the version so we skip re-download next time
$Version | Set-Content $VersionFile

# Cleanup
Remove-Item $TempExtract -Recurse -Force

# ── 5. Summary ────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "WebView2 SDK $Version installed successfully." -ForegroundColor Green
Write-Host ""
Write-Host "  Headers : Source/ThirdParty/WebView2/include/"
Write-Host "  Lib     : Source/ThirdParty/WebView2/lib/Win64/WebView2LoaderStatic.lib"
Write-Host ""
Write-Host "You can now build the InoWebUI plugin." -ForegroundColor Green
