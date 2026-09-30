param(
  [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
  if ([string]::IsNullOrWhiteSpace($env:GITHUB_WORKSPACE)) {
    throw "RepositoryRoot or GITHUB_WORKSPACE is required"
  }
  $RepositoryRoot = $env:GITHUB_WORKSPACE
}
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)

function Get-DependencyName($Dependency) {
  if ($Dependency -is [string]) { return $Dependency.ToLowerInvariant() }
  if ($null -ne $Dependency.name) { return ([string]$Dependency.name).ToLowerInvariant() }
  throw "vcpkg dependency entry has no name"
}

function Assert-GitGrepEmpty([string]$Pattern, [string[]]$Paths, [string]$Reason) {
  Push-Location $RepositoryRoot
  try {
    $args = @("grep", "-n", "-I", "-i", "-E", $Pattern, "--") + $Paths
    $output = & git @args 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 0) {
      throw ($Reason + [Environment]::NewLine + ($output -join [Environment]::NewLine))
    }
    if ($code -ne 1) {
      throw "git grep failed while auditing Studio boundaries"
    }
  } finally {
    Pop-Location
  }
}

$manifestPath = Join-Path $RepositoryRoot "vcpkg.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$rootDependencies = @($manifest.dependencies | ForEach-Object { Get-DependencyName $_ })
$guiDependencies = @("wtl", "scintilla", "lexilla")
foreach ($name in $guiDependencies) {
  if ($rootDependencies -contains $name) {
    throw "GUI dependency leaked into the default vcpkg closure: $name"
  }
}

if ($null -eq $manifest.features.app) { throw "vcpkg app feature is missing" }
$appDependencies = @($manifest.features.app.dependencies | ForEach-Object { Get-DependencyName $_ })
foreach ($name in $guiDependencies) {
  if ($appDependencies -notcontains $name) {
    throw "vcpkg app feature is missing required dependency: $name"
  }
}
if ($appDependencies -contains "chttp") {
  throw "chttp must not be mandatory for the base Studio app"
}

$optionsPath = Join-Path $RepositoryRoot "CMakeOptions.cmake"
$options = Get-Content -LiteralPath $optionsPath -Raw
if ($options -notmatch 'option\(TURBODB_BUILD_APP\s+"[^"]+"\s+OFF\)') {
  throw "TURBODB_BUILD_APP must remain OFF by default"
}
if ($options -notmatch '(?s)if\(TURBODB_BUILD_APP\).*?list\(APPEND VCPKG_MANIFEST_FEATURES app\)') {
  throw "vcpkg app feature is not gated by TURBODB_BUILD_APP"
}

$appCMake = Get-Content -LiteralPath (Join-Path $RepositoryRoot "app/CMakeLists.txt") -Raw
if ($appCMake -match 'chttp') {
  throw "Studio CMake must not acquire a mandatory chttp dependency"
}

Assert-GitGrepEmpty '(scintilla|lexilla|atlapp\.h|turbodb_app_editor_deps)' `
  @("orm", "dbtools", "drivers", "mysql", "redis", "tidesdb") `
  "GUI dependencies leaked outside the app boundary"

Assert-GitGrepEmpty '(#\s*include\s*[<"](sqlite3\.h|libpq-fe\.h|mysql\.h|mysql/mysql\.h)[>"])' `
  @("app/src", "app/tests") `
  "Studio code must not include provider-native database client headers"

Assert-GitGrepEmpty '(EXPLAIN QUERY PLAN|EXPLAIN \(FORMAT|EXPLAIN \(ANALYZE|information_schema|pg_catalog|sqlite_master)' `
  @("app/src", "app/tests") `
  "Studio code must not construct provider-specific SQL"

Write-Host "Studio dependency-boundary audit passed"
