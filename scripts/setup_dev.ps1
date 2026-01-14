# Scribe Development Environment Setup for Windows
# Run this script to install dependencies using vcpkg
# Requires: Git, Visual Studio 2019/2022 with C++ workload

param(
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg",
    [switch]$Help
)

if ($Help) {
    Write-Host "Usage: .\setup_dev.ps1 [OPTIONS]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -VcpkgRoot <path>  Custom vcpkg installation path (default: ~\vcpkg)"
    Write-Host "  -Help              Show this help"
    exit 0
}

$ErrorActionPreference = "Stop"

Write-Host "=== Scribe Development Setup (Windows) ===" -ForegroundColor Cyan
Write-Host ""

# Check for required tools
Write-Host "Checking prerequisites..." -ForegroundColor Yellow

# Check Git
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: Git is not installed. Please install Git from https://git-scm.com/" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] Git found" -ForegroundColor Green

# Check CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "  [!] CMake not found in PATH, will be installed with Visual Studio or separately" -ForegroundColor Yellow
} else {
    Write-Host "  [OK] CMake found" -ForegroundColor Green
}

# Check for Visual Studio
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasVS = $false
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -property installationPath 2>$null
    if ($vsPath) {
        Write-Host "  [OK] Visual Studio found at: $vsPath" -ForegroundColor Green
        $hasVS = $true
    }
}

if (-not $hasVS) {
    Write-Host "  [!] Visual Studio not found. You'll need VS 2019/2022 with C++ workload" -ForegroundColor Yellow
    Write-Host "      Download from: https://visualstudio.microsoft.com/" -ForegroundColor Yellow
}

Write-Host ""

# Install or update vcpkg
Write-Host "Setting up vcpkg package manager..." -ForegroundColor Yellow

if (-not (Test-Path $VcpkgRoot)) {
    Write-Host "  Cloning vcpkg to $VcpkgRoot..."
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot

    Write-Host "  Bootstrapping vcpkg..."
    Push-Location $VcpkgRoot
    .\bootstrap-vcpkg.bat -disableMetrics
    Pop-Location
} else {
    Write-Host "  vcpkg already installed at $VcpkgRoot"
    Write-Host "  Updating vcpkg..."
    Push-Location $VcpkgRoot
    git pull
    .\bootstrap-vcpkg.bat -disableMetrics
    Pop-Location
}

$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
Write-Host "  [OK] vcpkg ready" -ForegroundColor Green
Write-Host ""

# Install dependencies
Write-Host "Installing dependencies (this may take several minutes)..." -ForegroundColor Yellow
Write-Host ""

$triplet = "x64-windows"
$packages = @(
    "openssl:$triplet",
    "sqlite3:$triplet",
    "libpq:$triplet"
)

foreach ($package in $packages) {
    Write-Host "  Installing $package..."
    & $vcpkg install $package
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to install $package" -ForegroundColor Red
        exit 1
    }
}

# Integrate vcpkg with Visual Studio / CMake
Write-Host ""
Write-Host "Integrating vcpkg with build system..." -ForegroundColor Yellow
& $vcpkg integrate install

Write-Host ""
Write-Host "=== Dependencies installed successfully! ===" -ForegroundColor Green
Write-Host ""
Write-Host "To build Scribe, run:" -ForegroundColor Cyan
Write-Host "  .\scripts\build.ps1"
Write-Host ""
Write-Host "Or manually with CMake:" -ForegroundColor Cyan
Write-Host "  mkdir build-win; cd build-win"
Write-Host "  cmake .. -DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake"
Write-Host "  cmake --build . --config Release"
Write-Host ""

# Save vcpkg root for build script
$configFile = Join-Path $PSScriptRoot ".vcpkg_root"
$VcpkgRoot | Out-File -FilePath $configFile -Encoding UTF8 -NoNewline
Write-Host "vcpkg root saved to $configFile" -ForegroundColor Gray
