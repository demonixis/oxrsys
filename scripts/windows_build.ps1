# SPDX-License-Identifier: MPL-2.0

param(
    [ValidateSet("x64", "arm64")]
    [string]$Architecture = "x64",
    [string]$BuildDir = "",
    [string[]]$Target = @(),
    [string]$Config = ""
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

$targetNames = @()
foreach ($targetName in $Target) {
    foreach ($part in ($targetName -split ",")) {
        $trimmed = $part.Trim()
        if ($trimmed) {
            $targetNames += $trimmed
        }
    }
}

$buildArgs = @("--build", "`"$BuildDir`"")
if ($Config) {
    $buildArgs += "--config"
    $buildArgs += $Config
}
if ($targetNames.Count -gt 0) {
    $buildArgs += "--target"
    foreach ($targetName in $targetNames) {
        $buildArgs += $targetName
    }
}

$command = "call `"$vcvarsall`" $Architecture >nul && `"$cmake`" $($buildArgs -join ' ')"
Write-Host "Building OXRSys ($Architecture) with $cmake"
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
