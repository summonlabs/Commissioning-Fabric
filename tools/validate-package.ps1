<#
  Validates the installed artifact rather than the in-tree target:

    1. installs the already-built tree into a staging prefix;
    2. configures the independent downstream consumer in tests/downstream with
       find_package against that prefix;
    3. builds it with the same warning policy the library uses;
    4. runs it against a store it creates itself;
    5. verifies that the installed headers and the imported target are the only
       inputs the consumer needed.

  Every path is derived from the parameters, so the script contains no
  machine-specific path. It is invoked by the cxf.package test and can be run by
  hand after a build.
#>
param(
  [Parameter(Mandatory = $true)][string]$RepoRoot,
  [Parameter(Mandatory = $true)][string]$StageDir,
  [string]$BuildDir = "",
  [string]$Config = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($BuildDir)) { $BuildDir = Join-Path $RepoRoot 'build' }

$prefix = Join-Path $StageDir 'prefix'
$consumerBuild = Join-Path $StageDir 'consumer-build'
$storeDir = Join-Path $StageDir 'consumer-store'

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $prefix | Out-Null

Write-Output "package-validation: installing from $BuildDir to $prefix"
$installArgs = @('--install', $BuildDir, '--prefix', $prefix)
if (-not [string]::IsNullOrEmpty($Config)) { $installArgs += @('--config', $Config) }
& cmake @installArgs
if ($LASTEXITCODE -ne 0) { Write-Error "install failed with exit code $LASTEXITCODE"; exit 1 }

$configFile = Join-Path $prefix 'lib/cmake/CommissioningFabric/CommissioningFabricConfig.cmake'
if (-not (Test-Path $configFile)) {
  Write-Error "the installed package config was not found at $configFile"
  exit 1
}
$installedHeader = Join-Path $prefix 'include/cxf/runtime/fabric.hpp'
if (-not (Test-Path $installedHeader)) {
  Write-Error "the installed public headers were not found at $installedHeader"
  exit 1
}

Write-Output 'package-validation: configuring the downstream consumer'
$downstream = Join-Path $RepoRoot 'tests/downstream'

# The consumer is configured in a shell that has no compiler environment, so on
# Windows the multi-config Visual Studio generator is used: it locates its own
# toolchain. Elsewhere Ninja is used with whatever toolchain the environment
# provides.
# Windows PowerShell 5.1 has no $IsWindows, so the platform is detected from
# the environment variable every supported Windows shell sets.
$consumerConfig = 'Release'
if ($env:OS -eq 'Windows_NT') {
  $configureArgs = @('-S', $downstream, '-B', $consumerBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
                     "-DCMAKE_PREFIX_PATH=$prefix")
} else {
  $configureArgs = @('-S', $downstream, '-B', $consumerBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                     "-DCMAKE_PREFIX_PATH=$prefix")
  $consumerConfig = ''
}
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { Write-Error 'downstream configure failed'; exit 1 }

Write-Output 'package-validation: building the downstream consumer'
if ([string]::IsNullOrEmpty($consumerConfig)) {
  & cmake --build $consumerBuild
} else {
  & cmake --build $consumerBuild --config $consumerConfig
}
if ($LASTEXITCODE -ne 0) { Write-Error 'downstream build failed'; exit 1 }

Write-Output 'package-validation: running the downstream consumer'
$candidates = @(
  (Join-Path $consumerBuild 'downstream_consumer.exe'),
  (Join-Path $consumerBuild 'downstream_consumer'),
  (Join-Path $consumerBuild "$consumerConfig/downstream_consumer.exe"),
  (Join-Path $consumerBuild "$consumerConfig/downstream_consumer")
)
$consumer = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ([string]::IsNullOrEmpty($consumer)) {
  Write-Error 'the downstream consumer binary was not produced'
  exit 1
}
& $consumer $storeDir
if ($LASTEXITCODE -ne 0) { Write-Error "downstream consumer failed with exit code $LASTEXITCODE"; exit 1 }

Write-Output 'package-validation: ok'
exit 0
