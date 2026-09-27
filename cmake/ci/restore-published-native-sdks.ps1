param(
  [string]$SaltsVersion = "1.7.9",
  [string]$SaltsUtilsVersion = "4.0.1"
)

$ErrorActionPreference = "Stop"

if (-not $env:RUNNER_TEMP) {
  throw "RUNNER_TEMP is required"
}
if (-not $env:GITHUB_TOKEN) {
  throw "GITHUB_TOKEN with packages:read is required"
}

$packages = if ($env:QIGAO_NUGET_PACKAGES) {
  $env:QIGAO_NUGET_PACKAGES
} else {
  Join-Path $env:RUNNER_TEMP "qigao-nuget"
}

$config = Join-Path $env:RUNNER_TEMP "qigao-native-sdk.config"
$project = Join-Path $env:RUNNER_TEMP "qigao-native-sdk-restore.csproj"

@"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
  </packageSources>
</configuration>
"@ | Set-Content -LiteralPath $config -Encoding utf8NoBOM

dotnet nuget add source https://nuget.pkg.github.com/qigao/index.json `
  --name github `
  --username qigao `
  --password $env:GITHUB_TOKEN `
  --store-password-in-clear-text `
  --configfile $config

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesPath>$packages</RestorePackagesPath>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[$SaltsVersion]" />
    <PackageReference Include="SaltsUtils.Native" Version="[$SaltsUtilsVersion]" />
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8NoBOM

dotnet restore $project `
  --configfile $config `
  --packages $packages `
  --verbosity minimal

if ($IsWindows) {
  $rid = "windows-x64"
} elseif ($IsLinux) {
  $rid = "linux-x64"
} elseif ($IsMacOS) {
  $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
  $rid = if ($arch -eq "arm64") { "macos-arm64" } else { "macos-x64" }
} else {
  throw "unsupported host OS"
}

$saltsRoot = Join-Path $packages "salts.native/$SaltsVersion/sdk/$rid"
$saltsUtilsRoot = Join-Path $packages "saltsutils.native/$SaltsUtilsVersion/sdk/$rid"

$required = @(
  (Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $saltsRoot "include/cmeta/interface.h"),
  (Join-Path $saltsRoot "include/cmeta/function.h"),
  (Join-Path $saltsUtilsRoot "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $saltsUtilsRoot "include/salts/plugin.h"),
  (Join-Path $saltsUtilsRoot "include/data_bind.h")
)

foreach ($path in $required) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "published native SDK is incomplete: $path"
  }
}

$pluginHeader = Get-Content -LiteralPath (Join-Path $saltsUtilsRoot "include/salts/plugin.h") -Raw
if ($pluginHeader -notmatch '#define\s+SALTS_PLUGIN_ABI_VERSION\s+2u') {
  throw "SaltsUtils $SaltsUtilsVersion does not expose Plugin ABI 2"
}

if ($env:GITHUB_ENV) {
  "QIGAO_NUGET_PACKAGES=$packages" >> $env:GITHUB_ENV
  "SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
  "SALTS_UTILS_ROOT=$saltsUtilsRoot" >> $env:GITHUB_ENV
  "SALTS_SDK_VERSION=$SaltsVersion" >> $env:GITHUB_ENV
  "SALTS_UTILS_SDK_VERSION=$SaltsUtilsVersion" >> $env:GITHUB_ENV
}

if ($env:GITHUB_OUTPUT) {
  "salts_root=$saltsRoot" >> $env:GITHUB_OUTPUT
  "salts_utils_root=$saltsUtilsRoot" >> $env:GITHUB_OUTPUT
  "packages=$packages" >> $env:GITHUB_OUTPUT
  "rid=$rid" >> $env:GITHUB_OUTPUT
}

Write-Host "Salts.Native $SaltsVersion: $saltsRoot"
Write-Host "SaltsUtils.Native $SaltsUtilsVersion: $saltsUtilsRoot"
