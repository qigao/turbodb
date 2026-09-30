param(
  [Parameter(Mandatory = $true)]
  [string]$RepoRoot,

  [Parameter(Mandatory = $true)]
  [string]$VcpkgStatus
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not (Test-Path -LiteralPath $VcpkgStatus -PathType Leaf)) {
  throw "core-only vcpkg status file does not exist: $VcpkgStatus"
}
$VcpkgStatus = (Resolve-Path -LiteralPath $VcpkgStatus).Path

$manifestPath = Join-Path $RepoRoot "vcpkg.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

function Dependency-Names($Dependencies) {
  $names = @()
  foreach ($dependency in @($Dependencies)) {
    if ($dependency -is [string]) {
      $names += $dependency
    } elseif ($null -ne $dependency.name) {
      $names += [string]$dependency.name
    } else {
      throw "vcpkg dependency entry has no package name"
    }
  }
  return $names
}

$guiPackages = @("wtl", "scintilla", "lexilla")
$rootDependencies = Dependency-Names $manifest.dependencies
foreach ($package in $guiPackages) {
  if ($rootDependencies -contains $package) {
    throw "GUI package $package leaked into root vcpkg dependencies"
  }
}

$appFeature = $manifest.features.app
if ($null -eq $appFeature) {
  throw "vcpkg app feature is missing"
}
$appDependencies = Dependency-Names $appFeature.dependencies
foreach ($package in $guiPackages) {
  if ($appDependencies -notcontains $package) {
    throw "vcpkg app feature does not declare required GUI package $package"
  }
}
if ($appDependencies -contains "chttp") {
  throw "chttp must not be mandatory for the base TurboDB Studio feature"
}

$statusPackages = @()
foreach ($line in Get-Content -LiteralPath $VcpkgStatus) {
  if ($line -match '^Package:\s+(.+)$') {
    $statusPackages += $Matches[1].Trim()
  }
}
foreach ($package in $guiPackages) {
  if ($statusPackages -contains $package) {
    throw "core-only configure unexpectedly installed GUI package $package"
  }
}

$forbiddenBuildTokens = @(
  "wtl",
  "scintilla",
  "lexilla",
  "turbodb_app_editor_deps"
)
$boundaryFiles = @(
  (Join-Path $RepoRoot "orm/CMakeLists.txt"),
  (Join-Path $RepoRoot "dbtools/CMakeLists.txt")
)
$nestedBoundaryFiles = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "dbtools") -Recurse -File -Include "CMakeLists.txt","*.cmake" | Select-Object -ExpandProperty FullName
$boundaryFiles += $nestedBoundaryFiles

foreach ($file in $boundaryFiles | Select-Object -Unique) {
  $text = Get-Content -LiteralPath $file -Raw
  foreach ($token in $forbiddenBuildTokens) {
    if ($text.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
      throw "GUI dependency token '$token' leaked into core/dbtools CMake file: $file"
    }
  }
}

$appCMake = Get-Content -LiteralPath (Join-Path $RepoRoot "app/CMakeLists.txt") -Raw
if ($appCMake.IndexOf("chttp", [StringComparison]::OrdinalIgnoreCase) -ge 0) {
  throw "chttp must not be a direct TurboDB Studio build dependency"
}

Write-Host "Studio dependency boundary audit passed"
Write-Host "  core-only vcpkg packages: $($statusPackages.Count)"
Write-Host "  GUI packages remain app-feature scoped"
Write-Host "  orm/dbtools CMake remains free of WTL/Scintilla/Lexilla"
