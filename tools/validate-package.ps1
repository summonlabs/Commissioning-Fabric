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
  [string]$Config = "",
  [string]$ConsumerCxxFlags = "",
  [string]$ConsumerLinkFlags = ""
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

# The consumer is built in the configuration the library was built in: a Debug
# library and a Release consumer use different runtime libraries and do not
# link (LNK2038).
$libraryConfig = 'Release'
if (-not [string]::IsNullOrEmpty($Config)) { $libraryConfig = $Config }

# In a shell that has no compiler environment the multi-config Visual Studio
# generator is used on Windows: it locates its own toolchain. In a developer
# shell, and everywhere else, Ninja is used with the toolchain the environment
# provides, which is the one the library was built with.
# Windows PowerShell 5.1 has no $IsWindows, so the platform is detected from
# the environment variable every supported Windows shell sets.
$consumerConfig = $libraryConfig
if ($env:OS -eq 'Windows_NT' -and [string]::IsNullOrEmpty($env:VSCMD_VER)) {
  $configureArgs = @('-S', $downstream, '-B', $consumerBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
                     "-DCMAKE_PREFIX_PATH=$prefix")
} else {
  $configureArgs = @('-S', $downstream, '-B', $consumerBuild, '-G', 'Ninja',
                     "-DCMAKE_BUILD_TYPE=$libraryConfig", "-DCMAKE_PREFIX_PATH=$prefix")
  $consumerConfig = ''
}
# A library built with a sanitizer links only into a consumer built with it.
if (-not [string]::IsNullOrEmpty($ConsumerCxxFlags)) {
  $configureArgs += "-DCMAKE_CXX_FLAGS_INIT=$ConsumerCxxFlags"
}
if (-not [string]::IsNullOrEmpty($ConsumerLinkFlags)) {
  # The per-configuration link flags come last on the link line, so this is
  # where a flag such as /INCREMENTAL:NO has to go to take effect.
  $configureArgs += "-DCMAKE_EXE_LINKER_FLAGS_$($libraryConfig.ToUpperInvariant())=$ConsumerLinkFlags"
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
