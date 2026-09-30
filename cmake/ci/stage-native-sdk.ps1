param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64", "android-arm64-v8a")]
  [string]$Rid,
  [Parameter(Mandatory = $true)]
  [string]$Version
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
$platform = switch ($Rid) {
  "windows-x64" { "windows" }
  "linux-x64" { "linux" }
  "android-arm64-v8a" { "android" }
}
$manifest = Join-Path $env:GITHUB_WORKSPACE "build/ci-$platform-release/install_manifest.txt"
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "missing install manifest: $manifest" }
$prefix = [IO.Path]::GetFullPath($installed).Replace('\', '/').TrimEnd('/') + '/'
foreach ($file in Get-Content -LiteralPath $manifest | Sort-Object -Unique) {
  $absolute = [IO.Path]::GetFullPath($file).Replace('\', '/')
  if (-not $absolute.StartsWith($prefix, [StringComparison]::Ordinal)) {
    throw "installed file lies outside SDK prefix: $absolute"
  }
  $destination = Join-Path $stage $absolute.Substring($prefix.Length)
  New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
  Copy-Item -LiteralPath $file -Destination $destination
}

# A released Windows SDK carries its vcpkg DLL closure. Salts/SaltsUtils remain
# explicit NuGet dependencies; development builds keep using the preset PATH.
if ($Rid -eq "windows-x64") {
  Copy-Item -Path (Join-Path $env:GITHUB_WORKSPACE "vcpkg_installed/x64-windows/bin/*.dll") `
    -Destination (Join-Path $stage "bin")
}
@(
  "package=TurboDB.Native", "version=$Version", "rid=$Rid", "profile=release",
  "salts=$env:SALTS_PACKAGE_VERSION", "saltsutils=$env:SALTS_UTILS_PACKAGE_VERSION",
  "drivers=sqlite,postgresql,mysql,redis,tidesdb", "source=$env:GITHUB_SHA"
) | Set-Content -LiteralPath (Join-Path $stage "turbodb-sdk-manifest.txt") -Encoding utf8NoBOM
"TURBODB_ROOT=$($stage.Replace('\', '/'))" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
