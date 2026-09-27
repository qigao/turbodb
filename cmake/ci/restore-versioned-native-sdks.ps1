param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64")]
  [string]$Rid
)

$ErrorActionPreference = "Stop"

$saltsVersion = "1.7.9"
$saltsUtilsVersion = "4.0.1"

if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
  throw "GITHUB_TOKEN is required"
}
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
  throw "RUNNER_TEMP is required"
}
if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
  throw "GITHUB_ENV is required"
}
if ([string]::IsNullOrWhiteSpace($env:GITHUB_PATH)) {
  throw "GITHUB_PATH is required"
}

$packages = if ($env:QIGAO_NUGET_PACKAGES) {
  $env:QIGAO_NUGET_PACKAGES
} else {
  Join-Path $env:RUNNER_TEMP "qigao-nuget"
}
$config = Join-Path $env:RUNNER_TEMP "qigao-turbodb-native-sdk.config"
$project = Join-Path $env:RUNNER_TEMP "qigao-turbodb-native-sdk.csproj"

@'
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources><clear /></packageSources>
</configuration>
'@ | Set-Content -LiteralPath $config -Encoding utf8NoBOM

$sourceArgs = @(
  "nuget", "add", "source",
  "https://nuget.pkg.github.com/qigao/index.json",
  "--name", "github",
  "--username", "qigao",
  "--password", $env:GITHUB_TOKEN,
  "--store-password-in-clear-text",
  "--configfile", $config
)
& dotnet @sourceArgs
if ($LASTEXITCODE -ne 0) {
  throw "failed to configure qigao GitHub Packages source"
}

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[$saltsVersion]" />
    <PackageReference Include="SaltsUtils.Native" Version="[$saltsUtilsVersion]" />
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

$restoreArgs = @(
  "restore", $project,
  "--packages", $packages,
  "--configfile", $config,
  "--no-cache"
)
& dotnet @restoreArgs
if ($LASTEXITCODE -ne 0) {
  throw "failed to restore published native SDK pair"
}

$saltsPackage = Join-Path $packages "salts.native/$saltsVersion"
$saltsUtilsPackage = Join-Path $packages "saltsutils.native/$saltsUtilsVersion"
$saltsRoot = Join-Path $saltsPackage "sdk/$Rid"
$saltsUtilsRoot = Join-Path $saltsUtilsPackage "sdk/$Rid"

$required = @(
  (Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $saltsRoot "include/cmeta/interface.h"),
  (Join-Path $saltsRoot "include/cmeta/function.h"),
  (Join-Path $saltsUtilsRoot "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $saltsUtilsRoot "include/salts/plugin.h"),
  (Join-Path $saltsUtilsRoot "include/data_bind.h")
)
foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "published SDK is incomplete: $path"
  }
}

$pluginHeader = Get-Content -LiteralPath (Join-Path $saltsUtilsRoot "include/salts/plugin.h") -Raw
if ($pluginHeader -notmatch '#define\s+SALTS_PLUGIN_ABI_VERSION\s+2u') {
  throw "SaltsUtils $saltsUtilsVersion does not expose Plugin ABI 2"
}

"SALTS_ROOT=$saltsRoot" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_UTILS_ROOT=$saltsUtilsRoot" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"QIGAO_NUGET_PACKAGES=$packages" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_SDK_VERSION=$saltsVersion" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
"SALTS_UTILS_SDK_VERSION=$saltsUtilsVersion" | Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8

(Join-Path $saltsRoot "bin") | Add-Content -LiteralPath $env:GITHUB_PATH -Encoding utf8
(Join-Path $saltsUtilsRoot "bin") | Add-Content -LiteralPath $env:GITHUB_PATH -Encoding utf8

if ($Rid -eq "linux-x64") {
  $entries = @(
    (Join-Path $saltsRoot "lib"),
    (Join-Path $saltsUtilsRoot "lib")
  )
  if (-not [string]::IsNullOrWhiteSpace($env:LD_LIBRARY_PATH)) {
    $entries += $env:LD_LIBRARY_PATH
  }
  "LD_LIBRARY_PATH=$($entries -join ':')" |
    Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8
}

Write-Host "Restored Salts.Native $saltsVersion -> $saltsRoot"
Write-Host "Restored SaltsUtils.Native $saltsUtilsVersion -> $saltsUtilsRoot"
