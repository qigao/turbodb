param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64")]
  [string]$Rid,
  [string]$SdkRoot = ""
)

$ErrorActionPreference = "Stop"
foreach ($name in @("GITHUB_WORKSPACE", "TURBODB_CI_PKG_ROOT", "VCPKG_ROOT")) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "$name is required"
  }
}

$workspace = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$packageRoot = [IO.Path]::GetFullPath($env:TURBODB_CI_PKG_ROOT)
$source = Join-Path $workspace "packaging/tests/installed-consumer"
$build = Join-Path $workspace "build/installed-consumer-$Rid"
$triplet = if ($Rid -eq "windows-x64") { "x64-windows" } else { "x64-linux" }
$env:SALTS_ROOT = Join-Path $packageRoot "salts/release"
$env:SALTS_UTILS_ROOT = Join-Path $packageRoot "salts-utils/release"
$env:TURBODB_ROOT = if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
  Join-Path $packageRoot "turbodb/release"
} else {
  [IO.Path]::GetFullPath($SdkRoot)
}

foreach ($path in @($source, $env:SALTS_ROOT, $env:SALTS_UTILS_ROOT, $env:TURBODB_ROOT)) {
  if (-not (Test-Path -LiteralPath $path -PathType Container)) {
    throw "installed consumer input is missing: $path"
  }
}

$toolchain = $env:QIGAO_VCPKG_TOOLCHAIN_FILE
if ([string]::IsNullOrWhiteSpace($toolchain)) {
  $toolchain = Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"
}
$toolchain = [IO.Path]::GetFullPath($toolchain)
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
  throw "vcpkg toolchain is missing: $toolchain"
}

& cmake --fresh -S $source -B $build -G Ninja `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
  "-DVCPKG_MANIFEST_MODE=OFF" `
  "-DVCPKG_TARGET_TRIPLET=$triplet"
if ($LASTEXITCODE -ne 0) { throw "installed consumer configure failed" }

& cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw "installed consumer build failed" }

$executableName = if ($Rid -eq "windows-x64") {
  "turbodb_installed_consumer.exe"
} else {
  "turbodb_installed_consumer"
}
$executable = Join-Path $build $executableName
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
  throw "installed consumer executable is missing: $executable"
}

if ($Rid -eq "windows-x64") {
  $env:PATH = @(
    (Join-Path $env:TURBODB_ROOT "bin"),
    (Join-Path $env:SALTS_ROOT "bin"),
    (Join-Path $env:SALTS_UTILS_ROOT "bin"),
    [Environment]::SystemDirectory,
    $env:SystemRoot
  ) -join [IO.Path]::PathSeparator
} else {
  $runtimePaths = @(
    (Join-Path $env:TURBODB_ROOT "lib"),
    (Join-Path $env:SALTS_ROOT "lib"),
    (Join-Path $env:SALTS_UTILS_ROOT "lib")
  ) -join [IO.Path]::PathSeparator
  $env:LD_LIBRARY_PATH = if ([string]::IsNullOrEmpty($env:LD_LIBRARY_PATH)) {
    $runtimePaths
  } else {
    "$runtimePaths$([IO.Path]::PathSeparator)$env:LD_LIBRARY_PATH"
  }
}

& $executable
if ($LASTEXITCODE -ne 0) { throw "installed consumer runtime failed" }
