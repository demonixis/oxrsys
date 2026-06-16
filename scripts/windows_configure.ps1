# SPDX-License-Identifier: MPL-2.0

param(
    [ValidateSet("x64", "arm64")]
    [string]$Architecture = "x64",
    [string]$BuildType = "Debug",
    [string]$BuildDir = "",
    [string]$FFmpegRoot = "",
    [string]$VcpkgRoot = "",
    [switch]$NoVcpkg,
    [switch]$DynamicFFmpeg,
    [ValidateSet("AUTO", "ON", "OFF")]
    [string]$QtFrontends = "AUTO"
)

$ErrorActionPreference = "Stop"

function Find-FirstExistingFile {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Find-CommandOrFile {
    param(
        [string]$CommandName,
        [string[]]$Candidates
    )
    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return Find-FirstExistingFile $Candidates
}

$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build/windows-$Architecture"
}

$cmake = Find-CommandOrFile "cmake.exe" @(
    "$env:ProgramFiles\CMake\bin\cmake.exe",
    "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
if (-not $cmake) {
    throw "cmake.exe was not found. Install CMake or the Visual Studio CMake component."
}

$ninja = Find-CommandOrFile "ninja.exe" @(
    "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)
if (-not $ninja) {
    throw "ninja.exe was not found. Install Ninja or the Visual Studio CMake/Ninja component."
}

$vswhere = Find-FirstExistingFile @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
)
$vsInstall = $null
if ($vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsInstall) {
    $vsInstall = Find-FirstExistingFile @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\devenv.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.exe"
    )
    if ($vsInstall) {
        $vsInstall = Split-Path (Split-Path (Split-Path $vsInstall))
    }
}
if (-not $vsInstall) {
    throw "Visual Studio with MSVC tools was not found."
}

$vcvarsall = Join-Path $vsInstall "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path -LiteralPath $vcvarsall -PathType Leaf)) {
    throw "vcvarsall.bat was not found under $vsInstall."
}

$vcpkgToolchain = $null
if (-not $FFmpegRoot -and -not $NoVcpkg) {
    $vcpkgRootCandidates = @(
        $VcpkgRoot,
        $env:VCPKG_ROOT,
        (Join-Path $vsInstall "VC\vcpkg")
    )
    foreach ($candidate in $vcpkgRootCandidates) {
        if (-not $candidate) {
            continue
        }
        $toolchain = Join-Path $candidate "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain -PathType Leaf) {
            $vcpkgToolchain = (Resolve-Path -LiteralPath $toolchain).Path
            break
        }
    }
    if (-not $vcpkgToolchain) {
        throw "FFmpegRoot was not provided and vcpkg was not found. Pass -FFmpegRoot or install/point VCPKG_ROOT to vcpkg."
    }
}

if ($DynamicFFmpeg) {
    $vcpkgTriplet = if ($Architecture -eq "arm64") { "arm64-windows" } else { "x64-windows" }
    $ffmpegLinkMode = "DYNAMIC"
} else {
    $vcpkgTriplet = if ($Architecture -eq "arm64") { "arm64-windows-static-md" } else { "x64-windows-static-md" }
    $ffmpegLinkMode = "STATIC"
}

$configureArgs = @(
    "-S", "`"$repoRoot`"",
    "-B", "`"$BuildDir`"",
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DOXRSYS_BUILD_QT_FRONTENDS=$QtFrontends",
    "-DCMAKE_MAKE_PROGRAM=`"$ninja`""
)
if ($FFmpegRoot) {
    $configureArgs += "-DFFMPEG_ROOT=`"$FFmpegRoot`""
    $configureArgs += "-DOXRSYS_FFMPEG_LINK_MODE=AUTO"
} elseif ($vcpkgToolchain) {
    $configureArgs += "-UOXRSYS_FFMPEG_*"
    $configureArgs += "-UFFMPEG_*"
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=`"$vcpkgToolchain`""
    $configureArgs += "-DVCPKG_TARGET_TRIPLET=$vcpkgTriplet"
    $configureArgs += "-DOXRSYS_FFMPEG_LINK_MODE=$ffmpegLinkMode"
}

$command = "call `"$vcvarsall`" $Architecture >nul && `"$cmake`" $($configureArgs -join ' ')"
Write-Host "Configuring OXRSys ($Architecture) with $cmake"
if ($vcpkgToolchain) {
    Write-Host "Using vcpkg manifest FFmpeg via $vcpkgTriplet ($ffmpegLinkMode)"
}
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
