$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$NativeDir = Join-Path $Root 'native'
$SearchDir = Join-Path $Root 'search'
$BuildRoot = Join-Path $Root 'build'

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Name @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode"
    }
    return $exitCode
}

function Assert-Artifact {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required build artifact is missing: $Path"
    }
}

function Invoke-Smoke {
    param([Parameter(Mandatory = $true)][string]$Path)
    Assert-Artifact $Path
    & $Path
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Smoke test failed with exit code ${exitCode}: $Path"
    }
    return $exitCode
}


$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw 'CMake is required for the integrated build. Install CMake and rerun tools/build.ps1.'
}


Write-Host '[1/3] native build + tests'
$NativeBuild = Join-Path $BuildRoot 'native'
Invoke-Tool 'cmake' (@('-S', $NativeDir, '-B', $NativeBuild))
Invoke-Tool 'cmake' @('--build', $NativeBuild, '--config', 'Debug', '--parallel')
Invoke-Tool 'ctest' @('--test-dir', $NativeBuild, '--build-config', 'Debug', '--output-on-failure')

Write-Host '[2/3] native sanitizers'
$SanBuild = Join-Path $BuildRoot 'native-asan'
Invoke-Tool 'cmake' (@('-S', $NativeDir, '-B', $SanBuild, '-DNIYAH_ENABLE_ASAN=ON'))
Invoke-Tool 'cmake' @('--build', $SanBuild, '--config', 'Debug', '--parallel')
Invoke-Tool 'ctest' @('--test-dir', $SanBuild, '--build-config', 'Debug', '--output-on-failure')

Write-Host '[3/3] search build + smoke'
$SearchBuild = Join-Path $BuildRoot 'search'
Invoke-Tool 'cmake' (@('-S', $SearchDir, '-B', $SearchBuild))
Invoke-Tool 'cmake' @('--build', $SearchBuild, '--config', 'Debug', '--parallel')

Invoke-Tool 'ctest' @('--test-dir', $SearchBuild, '--build-config', 'Debug', '--output-on-failure')
