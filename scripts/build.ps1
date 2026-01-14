# Scribe Build Script for Windows
# Builds Scribe using CMake and vcpkg dependencies

param(
    [switch]$Debug,
    [switch]$NoPostgres,
    [switch]$Test,
    [switch]$Clean,
    [string]$VcpkgRoot,
    [switch]$Help
)

if ($Help) {
    Write-Host "Usage: .\build.ps1 [OPTIONS]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Debug         Build with debug symbols"
    Write-Host "  -NoPostgres    Build without PostgreSQL support"
    Write-Host "  -Test          Run tests after build"
    Write-Host "  -Clean         Clean build directory first"
    Write-Host "  -VcpkgRoot     Path to vcpkg installation"
    Write-Host "  -Help          Show this help"
    exit 0
}

$ErrorActionPreference = "Stop"

# Determine paths
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectDir "build-win"

# Find vcpkg root
if (-not $VcpkgRoot) {
    $configFile = Join-Path $ScriptDir ".vcpkg_root"
    if (Test-Path $configFile) {
        $VcpkgRoot = Get-Content $configFile -Raw
        $VcpkgRoot = $VcpkgRoot.Trim()
    } elseif (Test-Path "$env:USERPROFILE\vcpkg") {
        $VcpkgRoot = "$env:USERPROFILE\vcpkg"
    } else {
        Write-Host "ERROR: vcpkg not found. Run setup_dev.ps1 first or specify -VcpkgRoot" -ForegroundColor Red
        exit 1
    }
}

$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $VcpkgToolchain)) {
    Write-Host "ERROR: vcpkg toolchain not found at $VcpkgToolchain" -ForegroundColor Red
    Write-Host "Run setup_dev.ps1 to install vcpkg and dependencies" -ForegroundColor Yellow
    exit 1
}

# Build configuration
$BuildType = if ($Debug) { "Debug" } else { "Release" }
$WithPostgres = if ($NoPostgres) { "OFF" } else { "ON" }
$RunTests = if ($Test) { "ON" } else { "OFF" }

Write-Host "=== Scribe Build (Windows) ===" -ForegroundColor Cyan
Write-Host "  Build type:   $BuildType"
Write-Host "  PostgreSQL:   $WithPostgres"
Write-Host "  vcpkg:        $VcpkgRoot"
Write-Host ""

# Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# Create build directory
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Push-Location $BuildDir

try {
    # Configure with CMake
    Write-Host "Configuring..." -ForegroundColor Yellow

    $cmakeArgs = @(
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DSCRIBE_WITH_POSTGRES=$WithPostgres",
        "-DSCRIBE_BUILD_TESTS=$RunTests",
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        $ProjectDir
    )

    # Detect generator - prefer Ninja if available, fallback to VS
    if (Get-Command ninja -ErrorAction SilentlyContinue) {
        $cmakeArgs = @("-G", "Ninja") + $cmakeArgs
        Write-Host "  Using Ninja generator" -ForegroundColor Gray
    } else {
        # Auto-detect Visual Studio version
        $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vsWhere) {
            $vsVersion = & $vsWhere -latest -property catalog_productLineVersion 2>$null
            if ($vsVersion -eq "2022") {
                $cmakeArgs = @("-G", "Visual Studio 17 2022", "-A", "x64") + $cmakeArgs
                Write-Host "  Using Visual Studio 2022 generator" -ForegroundColor Gray
            } elseif ($vsVersion -eq "2019") {
                $cmakeArgs = @("-G", "Visual Studio 16 2019", "-A", "x64") + $cmakeArgs
                Write-Host "  Using Visual Studio 2019 generator" -ForegroundColor Gray
            }
        }
    }

    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }

    # Build
    Write-Host ""
    Write-Host "Building..." -ForegroundColor Yellow

    $cpuCount = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    cmake --build . --config $BuildType --parallel $cpuCount
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }

    # Run tests if requested
    if ($Test) {
        Write-Host ""
        Write-Host "Running tests..." -ForegroundColor Yellow
        ctest --output-on-failure --build-config $BuildType
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed"
        }
    }

    Write-Host ""
    Write-Host "=== Build complete! ===" -ForegroundColor Green

    # Find the built executable
    $exePath = Join-Path $BuildDir "bin\$BuildType\scribe.exe"
    if (-not (Test-Path $exePath)) {
        $exePath = Join-Path $BuildDir "bin\scribe.exe"
    }

    if (Test-Path $exePath) {
        Write-Host "Binary: $exePath" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "To run scribe:" -ForegroundColor Yellow
        Write-Host "  $exePath --help"
    } else {
        Write-Host "Binary: $BuildDir\bin\scribe.exe" -ForegroundColor Cyan
    }

    Write-Host ""
    Write-Host "To add to PATH temporarily:" -ForegroundColor Yellow
    Write-Host "  `$env:PATH = `"$BuildDir\bin\$BuildType;`$env:PATH`""
    Write-Host ""

} finally {
    Pop-Location
}
