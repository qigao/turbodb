param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "linux-arm64", "macos-arm64", "windows-x64", "android-arm64-v8a")]
  [string]$Rid,
  [ValidateSet("linux-x64", "linux-arm64", "macos-arm64", "windows-x64")]
  [string]$HostRid,
  [switch]$Local
)

$ErrorActionPreference = "Stop"
$requiredEnvironment = @("GITHUB_TOKEN")
if (-not $Local) {
  $requiredEnvironment += @("RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH", "GITHUB_WORKSPACE")
}
foreach ($name in $requiredEnvironment) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "$name is required"
  }
}
if (-not $HostRid) {
  if ($Rid -eq "android-arm64-v8a") { throw "HostRid is required for cross compilation" }
  $HostRid = $Rid
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$restoreRoot = if ($Local) { Join-Path $repositoryRoot "build/native-sdk" } else { $env:RUNNER_TEMP }
$packages = if ($env:QIGAO_NUGET_PACKAGES) {
  $env:QIGAO_NUGET_PACKAGES
} elseif ($Local) {
  Join-Path $repositoryRoot "stage/nuget"
} else {
  Join-Path $restoreRoot "qigao-nuget"
}
$packages = [IO.Path]::GetFullPath($packages)
$config = Join-Path $repositoryRoot "cmake/vcpkg-cache.nuget.config"
$project = Join-Path $restoreRoot "qigao-turbodb-native-sdk.csproj"
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="2.3.0-*" />
    <PackageReference Include="SaltsUtils.Native" Version="4.3.0-*" />
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "failed to restore latest published SDKs and host tools" }
$assets = Get-Content -LiteralPath (Join-Path $restoreRoot "obj/project.assets.json") -Raw |
  ConvertFrom-Json -AsHashtable
function Get-RestoredPackage([string]$name) {
  $keys = @($assets.libraries.Keys | Where-Object {
    $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase)
  })
  if ($keys.Count -ne 1) { throw "expected one resolved $name package" }
  return Join-Path $packages $assets.libraries[$keys[0]].path
}
$saltsPackage = Get-RestoredPackage "Salts.Native"
$utilsPackage = Get-RestoredPackage "SaltsUtils.Native"
$saltsRoot = Join-Path $saltsPackage "sdk/$Rid"
$utilsRoot = Join-Path $utilsPackage "sdk/$Rid"
$re2cRoot = Join-Path (Get-RestoredPackage "Qigao.Re2c.Binary") "tools/$HostRid"
$re2cExe = if ($HostRid -eq "windows-x64") { "re2c.exe" } else { "re2c" }
foreach ($path in @(
  (Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $utilsRoot "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $re2cRoot "bin/$re2cExe")
)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing restored SDK file: $path" }
}
if (-not $IsWindows) {
  & chmod +x (Join-Path $re2cRoot "bin/$re2cExe")
  if ($LASTEXITCODE -ne 0) { throw "failed to make restored re2c executable" }
}

if ($Local) {
  function Set-LocalPackageLink([string]$relativePath, [string]$target) {
    $link = Join-Path $repositoryRoot "stage/dependencies/$relativePath"
    New-Item -ItemType Directory -Path (Split-Path $link -Parent) -Force | Out-Null
    $existing = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($existing) {
      $expectedType = if ($IsWindows) { "Junction" } else { "SymbolicLink" }
      if ($existing.LinkType -ne $expectedType) { throw "refusing to replace non-link SDK path: $link" }
      if ($existing.Target -eq $target) { return $link }
      Remove-Item -LiteralPath $link -Force
    }
    $linkType = if ($IsWindows) { "Junction" } else { "SymbolicLink" }
    New-Item -ItemType $linkType -Path $link -Target $target | Out-Null
    return $link
  }
  $saltsRoot = Set-LocalPackageLink "salts/$Rid" $saltsRoot
  $utilsRoot = Set-LocalPackageLink "salts-utils/$Rid" $utilsRoot
  $re2cRoot = Set-LocalPackageLink "re2c/$HostRid" $re2cRoot
}

$environment = [ordered]@{
  SALTS_ROOT = $saltsRoot
  SALTS_UTILS_ROOT = $utilsRoot
  RE2C_ROOT = $re2cRoot
  QIGAO_NUGET_PACKAGES = $packages
  SALTS_PACKAGE_VERSION = (Split-Path $saltsPackage -Leaf)
  SALTS_UTILS_PACKAGE_VERSION = (Split-Path $utilsPackage -Leaf)
}
if (-not $Local) {
  $environment.TURBODB_CI_PKG_ROOT = Join-Path $env:GITHUB_WORKSPACE "external/pkgs"
}
foreach ($entry in $environment.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
if (-not $Local) { (Join-Path $re2cRoot "bin") >> $env:GITHUB_PATH }
Write-Host "Restored Salts.Native $($environment.SALTS_PACKAGE_VERSION) and SaltsUtils.Native $($environment.SALTS_UTILS_PACKAGE_VERSION) for $Rid"
