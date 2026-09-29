param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64")]
  [string]$Rid
)

$ErrorActionPreference = "Stop"

foreach ($name in @("GITHUB_TOKEN", "RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH")) {
  $value = [Environment]::GetEnvironmentVariable($name)
  if ([string]::IsNullOrWhiteSpace($value)) { throw "$name is required" }
}

$packages = if ($env:QIGAO_NUGET_PACKAGES) {
  $env:QIGAO_NUGET_PACKAGES
} else {
  Join-Path $env:RUNNER_TEMP "qigao-nuget"
}
$config = Join-Path $env:RUNNER_TEMP "qigao-turbodb-native-sdk.config"
$project = Join-Path $env:RUNNER_TEMP "qigao-turbodb-native-sdk.csproj"
$assetsPath = Join-Path $env:RUNNER_TEMP "obj/project.assets.json"

@'
<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /></packageSources></configuration>
'@ | Set-Content -LiteralPath $config -Encoding utf8NoBOM

$sourceArgs = @(
  "nuget", "add", "source", "https://nuget.pkg.github.com/qigao/index.json",
  "--name", "github",
  "--username", "qigao",
  "--password", $env:GITHUB_TOKEN,
  "--store-password-in-clear-text",
  "--configfile", $config
)
& dotnet @sourceArgs
if ($LASTEXITCODE -ne 0) { throw "failed to configure qigao GitHub Packages source" }

@'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
    <PackageReference Include="SaltsUtils.Native" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

& dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "failed to restore latest published native SDK pair" }
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
  throw "NuGet restore did not produce $assetsPath"
}

$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json
function Get-ResolvedPackageVersion([string]$PackageId) {
  $prefix = "$PackageId/"
  foreach ($property in $assets.libraries.PSObject.Properties) {
    if ($property.Name.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
      return $property.Name.Substring($prefix.Length)
    }
  }
  throw "NuGet restore did not resolve $PackageId"
}

$saltsVersion = Get-ResolvedPackageVersion "Salts.Native"
$saltsUtilsVersion = Get-ResolvedPackageVersion "SaltsUtils.Native"
$saltsRoot = Join-Path $packages "salts.native/$saltsVersion/sdk/$Rid"
$saltsUtilsRoot = Join-Path $packages "saltsutils.native/$saltsUtilsVersion/sdk/$Rid"

$required = @(
  (Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $saltsRoot "include/cmeta/interface.h"),
  (Join-Path $saltsRoot "include/cmeta/function.h"),
  (Join-Path $saltsRoot "include/salts/plugin.h"),
  (Join-Path $saltsUtilsRoot "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $saltsUtilsRoot "include/data_bind.h")
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "published SDK is incomplete: $path"
  }
}

$pluginHeader = Get-Content -LiteralPath (Join-Path $saltsRoot "include/salts/plugin.h") -Raw
if ($pluginHeader -notmatch '#define\s+SALTS_PLUGIN_ABI_VERSION\s+2u') {
  throw "resolved Salts package does not expose Plugin ABI 2"
}

$dataBindHeader = Get-Content -LiteralPath (Join-Path $saltsUtilsRoot "include/data_bind.h") -Raw
if ($dataBindHeader -notmatch '#define\s+DATA_BIND_VERSION_MAJOR\s+3') {
  throw "resolved SaltsUtils package does not expose DataBind 3"
}
if ($dataBindHeader -notmatch '#define\s+DATA_BIND_ABI_VERSION\s+9') {
  throw "resolved SaltsUtils package does not expose DataBind ABI 9"
}

"SALTS_ROOT=$saltsRoot" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_UTILS_ROOT=$saltsUtilsRoot" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"QIGAO_NUGET_PACKAGES=$packages" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_PACKAGE_VERSION=$saltsVersion" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_UTILS_PACKAGE_VERSION=$saltsUtilsVersion" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8

(Join-Path $saltsRoot "bin") | Add-Content -LiteralPath $env:GITHUB_PATH -Encoding utf8
(Join-Path $saltsUtilsRoot "bin") | Add-Content -LiteralPath $env:GITHUB_PATH -Encoding utf8

if ($Rid -eq "linux-x64") {
  $entries = @((Join-Path $saltsRoot "lib"), (Join-Path $saltsUtilsRoot "lib"))
  if (-not [string]::IsNullOrWhiteSpace($env:LD_LIBRARY_PATH)) {
    $entries += $env:LD_LIBRARY_PATH
  }
  "LD_LIBRARY_PATH=$($entries -join ':')" |
    Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
}

Write-Host "Restored latest Salts.Native -> $saltsVersion -> $saltsRoot"
Write-Host "Restored latest SaltsUtils.Native -> $saltsUtilsVersion -> $saltsUtilsRoot"
