param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "linux-arm64", "macos-arm64", "windows-x64", "android-arm64-v8a")]
  [string]$Rid,
  [Parameter(Mandatory = $true)]
  [string]$Version,
  [Parameter(Mandatory = $true)]
  [string]$InstallManifest
)

$ErrorActionPreference = "Stop"
foreach ($name in @("GITHUB_WORKSPACE", "TURBODB_CI_PKG_ROOT", "GITHUB_ENV",
                    "SALTS_PACKAGE_VERSION", "SALTS_UTILS_PACKAGE_VERSION")) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "$name is required"
  }
}
$package = if ($Rid -eq "android-arm64-v8a") { "turbodb-android" } else { "turbodb" }
$installed = Join-Path $env:TURBODB_CI_PKG_ROOT "$package/release"
$stage = Join-Path $env:GITHUB_WORKSPACE "build/stage/sdk/$Rid"
if (Test-Path -LiteralPath $stage) { throw "SDK staging directory already exists: $stage" }
if (-not (Test-Path -LiteralPath $installed -PathType Container)) { throw "install tree is missing: $installed" }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
if (-not (Test-Path -LiteralPath $InstallManifest -PathType Leaf)) {
  throw "missing install manifest: $InstallManifest"
}
$prefix = [IO.Path]::GetFullPath($installed).Replace('\', '/').TrimEnd('/') + '/'
foreach ($file in Get-Content -LiteralPath $InstallManifest | Sort-Object -Unique) {
  $absolute = [IO.Path]::GetFullPath($file).Replace('\', '/')
  if (-not $absolute.StartsWith($prefix, [StringComparison]::Ordinal)) {
    throw "installed file lies outside SDK prefix: $absolute"
  }
  $destination = Join-Path $stage $absolute.Substring($prefix.Length)
  New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
  Copy-Item -LiteralPath $file -Destination $destination
}

# Keep this list to the runtime closure of the shipped SQLite and PostgreSQL
# drivers. Copying the entire vcpkg bin directory leaks unrelated tools and
# libraries into the SDK. TLS and authentication digests use Salts; libpq is
# built without its own SSL provider.
if ($Rid -eq "windows-x64") {
  $vcpkgBin = Join-Path $env:GITHUB_WORKSPACE "vcpkg_installed/$env:QIGAO_VCPKG_WINDOWS_TRIPLET/bin"
  $driverDirectory = Join-Path $stage "lib/turbodb/drivers"
  if (-not (Test-Path -LiteralPath $driverDirectory -PathType Container)) {
    throw "installed Windows driver directory is missing: $driverDirectory"
  }
  foreach ($runtimeDll in @("libpq.dll", "sqlite3.dll")) {
    $source = Join-Path $vcpkgBin $runtimeDll
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
      throw "required Windows driver runtime is missing: $source"
    }
    # dbtools links the database clients directly, while Salts Plugin loads an
    # explicit driver path with DLL_LOAD_DIR | DEFAULT_DIRS. Keep one copy in
    # bin for executables and one beside the driver modules for secure loading.
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage "bin")
    Copy-Item -LiteralPath $source -Destination $driverDirectory
  }
}
$runtimeManifest = @()
if ($Rid -in @("linux-x64", "linux-arm64", "android-arm64-v8a")) {
  if ([string]::IsNullOrWhiteSpace($env:SALTS_ROOT)) { throw "SALTS_ROOT is required" }
  $saltsLibrary = Join-Path $env:SALTS_ROOT "lib/libsalts.so"
  if (-not (Test-Path -LiteralPath $saltsLibrary -PathType Leaf)) {
    throw "restored Salts runtime is missing: $saltsLibrary"
  }
  # Package versions do not define the runtime ABI; record the SDK's ELF SONAME.
  $dynamic = & readelf -d $saltsLibrary
  if ($LASTEXITCODE -ne 0) { throw "failed to read Salts runtime SONAME: $saltsLibrary" }
  $sonames = @($dynamic | ForEach-Object {
    if ($_ -match '\(SONAME\).*\[([^\]]+)\]') { $Matches[1] }
  })
  if ($sonames.Count -ne 1) { throw "expected one Salts runtime SONAME: $saltsLibrary" }
  $runtimeManifest += "salts_soname=$($sonames[0])"
}
@(
  "package=TurboDB.Native", "version=$Version", "rid=$Rid", "profile=release",
  "salts=$env:SALTS_PACKAGE_VERSION", "saltsutils=$env:SALTS_UTILS_PACKAGE_VERSION",
  "drivers=sqlite,postgresql,mysql", "source=$env:GITHUB_SHA",
  "tidessql=OFF", "tidesdb=OFF"
  $runtimeManifest
) | Set-Content -LiteralPath (Join-Path $stage "turbodb-sdk-manifest.txt") -Encoding utf8NoBOM
"TURBODB_ROOT=$($stage.Replace('\', '/'))" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
