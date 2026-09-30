param(
  [Parameter(Mandatory = $true)]
  [string]$StudioExe,

  [Parameter(Mandatory = $true)]
  [string]$StageDir,

  [Parameter(Mandatory = $true)]
  [string]$VcpkgBin,

  [Parameter(Mandatory = $true)]
  [string]$SaltsBin,

  [Parameter(Mandatory = $true)]
  [string]$SaltsUtilsBin
)

$ErrorActionPreference = "Stop"

function Require-Directory([string]$Path, [string]$Role) {
  if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
    throw "$Role directory does not exist: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

function Require-File([string]$Path, [string]$Role) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "$Role file does not exist: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

$StudioExe = Require-File $StudioExe "Studio executable"
$VcpkgBin = Require-Directory $VcpkgBin "vcpkg runtime"
$SaltsBin = Require-Directory $SaltsBin "Salts runtime"
$SaltsUtilsBin = Require-Directory $SaltsUtilsBin "SaltsUtils runtime"

if ([string]::IsNullOrWhiteSpace($env:VSINSTALL)) {
  throw "VSINSTALL is required to locate dumpbin.exe"
}

$msvcRoot = Join-Path $env:VSINSTALL "VC/Tools/MSVC"
$toolset = Get-ChildItem -LiteralPath $msvcRoot -Directory |
  Sort-Object Name -Descending |
  Select-Object -First 1
if ($null -eq $toolset) {
  throw "MSVC toolset directory was not found under $msvcRoot"
}
$dumpbin = Join-Path $toolset.FullName "bin/Hostx64/x64/dumpbin.exe"
$dumpbin = Require-File $dumpbin "dumpbin"

$buildBin = Split-Path -Parent $StudioExe
$system32 = Require-Directory (Join-Path $env:SystemRoot "System32") "Windows System32"
$searchDirs = @(
  $buildBin,
  $VcpkgBin,
  $SaltsBin,
  $SaltsUtilsBin,
  $system32
)

if (Test-Path -LiteralPath $StageDir) {
  Remove-Item -LiteralPath $StageDir -Recurse -Force
}
$stageBin = Join-Path $StageDir "bin"
New-Item -ItemType Directory -Path $stageBin -Force | Out-Null

function Get-ImportedDlls([string]$Binary) {
  $output = & $dumpbin /nologo /dependents $Binary 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $Binary: $($output -join '; ')"
  }

  $names = @()
  foreach ($line in $output) {
    $text = [string]$line
    if ($text -match '^\s+([A-Za-z0-9_.+\-]+\.dll)\s*$') {
      $names += $Matches[1]
    }
  }
  return $names
}

function Resolve-RuntimeDll([string]$Name) {
  foreach ($directory in $searchDirs) {
    $candidate = Join-Path $directory $Name
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  return $null
}

function Is-SystemDll([string]$Path) {
  return $Path.StartsWith(
    $system32 + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)
}

$queue = [System.Collections.Generic.Queue[string]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new(
  [StringComparer]::OrdinalIgnoreCase)
$manifest = [System.Collections.Generic.List[string]]::new()

$studioName = Split-Path -Leaf $StudioExe
Copy-Item -LiteralPath $StudioExe -Destination (Join-Path $stageBin $studioName)
$queue.Enqueue($StudioExe)
$seen.Add($studioName) | Out-Null
$manifest.Add("$studioName <- $StudioExe")

# Scintilla and Lexilla are loaded explicitly with LoadLibraryW, so they do not
# have to appear in the executable's static import table. They are deliberate
# runtime seeds, not an implicit scan of the vcpkg bin directory.
foreach ($runtimeName in @("Scintilla.dll", "Lexilla.dll")) {
  $runtime = Require-File (Join-Path $VcpkgBin $runtimeName) $runtimeName
  Copy-Item -LiteralPath $runtime -Destination (Join-Path $stageBin $runtimeName)
  if ($seen.Add($runtimeName)) {
    $queue.Enqueue($runtime)
    $manifest.Add("$runtimeName <- $runtime")
  }
}

while ($queue.Count -ne 0) {
  $binary = $queue.Dequeue()
  foreach ($dependency in Get-ImportedDlls $binary) {
    if ($dependency -match '^(api-ms-win-|ext-ms-win-)') {
      continue
    }
    if ($dependency -match '^turbodb_driver_.*\.dll$') {
      throw "Studio runtime closure unexpectedly depends on driver plugin $dependency"
    }
    if ($seen.Contains($dependency)) {
      continue
    }

    $resolved = Resolve-RuntimeDll $dependency
    if ($null -eq $resolved) {
      throw "Unable to resolve runtime dependency $dependency required by $binary"
    }
    if (Is-SystemDll $resolved) {
      $seen.Add($dependency) | Out-Null
      continue
    }

    $destination = Join-Path $stageBin $dependency
    Copy-Item -LiteralPath $resolved -Destination $destination
    $seen.Add($dependency) | Out-Null
    $queue.Enqueue($resolved)
    $manifest.Add("$dependency <- $resolved")
  }
}

$unexpectedDrivers = Get-ChildItem -LiteralPath $stageBin -Filter "turbodb_driver_*.dll" -File
if ($unexpectedDrivers.Count -ne 0) {
  throw "Driver plugins must not be bundled in the base Studio package"
}

$manifestPath = Join-Path $StageDir "runtime-manifest.txt"
$manifest | Sort-Object | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM

Write-Host "TurboDB Studio runtime staged at $StageDir"
Get-ChildItem -LiteralPath $stageBin -File |
  Sort-Object Name |
  ForEach-Object { Write-Host "  $($_.Name)" }
