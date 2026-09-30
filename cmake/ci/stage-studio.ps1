param(
  [string]$BuildDir = "",
  [string]$StageDir = "",
  [string]$ArchivePath = ""
)

$ErrorActionPreference = "Stop"

foreach ($name in @("GITHUB_WORKSPACE", "SALTS_ROOT", "SALTS_UTILS_ROOT", "VSINSTALL")) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "$name is required"
  }
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $env:GITHUB_WORKSPACE "build/ci-windows-release"
}
if ([string]::IsNullOrWhiteSpace($StageDir)) {
  $StageDir = Join-Path $env:GITHUB_WORKSPACE "build/stage/studio/windows-x64"
}
if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
  $ArchivePath = Join-Path $env:GITHUB_WORKSPACE "build/stage/TurboDBStudio-windows-x64.zip"
}

$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$StageDir = [IO.Path]::GetFullPath($StageDir)
$ArchivePath = [IO.Path]::GetFullPath($ArchivePath)

$exe = Join-Path $BuildDir "bin/TurboDBStudio.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
  throw "TurboDB Studio executable is missing: $exe"
}

$vcpkgBin = Join-Path $env:GITHUB_WORKSPACE "vcpkg_installed/x64-windows/bin"
$saltsBin = Join-Path $env:SALTS_ROOT "bin"
$saltsUtilsBin = Join-Path $env:SALTS_UTILS_ROOT "bin"
$buildBin = Join-Path $BuildDir "bin"

$toolsetRoot = Join-Path $env:VSINSTALL "VC/Tools/MSVC"
$toolset = Get-ChildItem -LiteralPath $toolsetRoot -Directory |
  Sort-Object Name -Descending |
  Select-Object -First 1
if ($null -eq $toolset) { throw "MSVC toolset was not found under $toolsetRoot" }
$dumpbin = Join-Path $toolset.FullName "bin/Hostx64/x64/dumpbin.exe"
if (-not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
  throw "dumpbin.exe is missing: $dumpbin"
}

$redistRoot = Join-Path $env:VSINSTALL "VC/Redist/MSVC"
$redist = Get-ChildItem -LiteralPath $redistRoot -Directory |
  Sort-Object Name -Descending |
  ForEach-Object { Join-Path $_.FullName "x64/Microsoft.VC143.CRT" } |
  Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
  Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($redist)) {
  throw "MSVC x64 runtime redistribution directory was not found"
}

$allowedRoots = @($buildBin, $vcpkgBin, $saltsBin, $saltsUtilsBin, $redist) |
  ForEach-Object { [IO.Path]::GetFullPath($_) }
foreach ($root in $allowedRoots) {
  if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "required Studio runtime root is missing: $root"
  }
}

$systemDir = [Environment]::SystemDirectory

function Get-Dependents([string]$Binary) {
  $output = & $dumpbin /nologo /dependents $Binary 2>&1
  if ($LASTEXITCODE -ne 0) {
    $joined = $output -join [Environment]::NewLine
    throw ("dumpbin failed for " + $Binary + [Environment]::NewLine + $joined)
  }
  $collect = $false
  $result = [System.Collections.Generic.List[string]]::new()
  foreach ($line in $output) {
    $text = [string]$line
    if ($text -match "Image has the following dependencies") {
      $collect = $true
      continue
    }
    if (-not $collect) { continue }
    if ($text -match "^\s*Summary\s*$") { break }
    if ($text -match "^\s+([A-Za-z0-9_.+\-]+\.dll)\s*$") {
      $result.Add($Matches[1])
    }
  }
  return @($result | Sort-Object -Unique)
}

function Resolve-Dependency([string]$Name) {
  if ($Name -like "api-ms-win-*.dll" -or $Name -like "ext-ms-*.dll") {
    return $null
  }
  if ($Name -match "^(?i:turbodb_driver_.*\.dll)$") {
    throw "Studio acquired an implicit runtime Driver dependency: $Name"
  }

  $matches = [System.Collections.Generic.List[string]]::new()
  foreach ($root in $allowedRoots) {
    $candidate = Join-Path $root $Name
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $matches.Add([IO.Path]::GetFullPath($candidate))
    }
  }

  if ($matches.Count -gt 0) {
    $unique = @($matches | Sort-Object -Unique)
    if ($unique.Count -gt 1) {
      $hashes = @($unique | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
      } | Sort-Object -Unique)
      if ($hashes.Count -ne 1) {
        throw "ambiguous Studio runtime dependency $Name resolved to different binaries: $($unique -join ', ')"
      }
    }
    return $unique[0]
  }

  $system = Join-Path $systemDir $Name
  if (Test-Path -LiteralPath $system -PathType Leaf) {
    return $null
  }
  throw "unresolved Studio runtime dependency: $Name"
}

if (Test-Path -LiteralPath $StageDir) {
  throw "Studio staging directory already exists: $StageDir"
}
if (Test-Path -LiteralPath $ArchivePath) {
  throw "Studio archive already exists: $ArchivePath"
}
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $ArchivePath) -Force | Out-Null

$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue([IO.Path]::GetFullPath($exe))
foreach ($requiredEditorDll in @("Scintilla.dll", "Lexilla.dll")) {
  $resolved = Resolve-Dependency $requiredEditorDll
  if ([string]::IsNullOrWhiteSpace($resolved)) {
    throw "required editor runtime resolved as a system DLL: $requiredEditorDll"
  }
  $queue.Enqueue($resolved)
}

$copied = @{}
while ($queue.Count -gt 0) {
  $binary = $queue.Dequeue()
  $name = [IO.Path]::GetFileName($binary)
  $key = $name.ToLowerInvariant()
  if ($copied.ContainsKey($key)) {
    $existingHash = (Get-FileHash -LiteralPath $copied[$key] -Algorithm SHA256).Hash
    $newHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    if ($existingHash -ne $newHash) {
      throw "Studio package basename collision for $name"
    }
    continue
  }
  if ($name -match "^(?i:turbodb_driver_.*\.dll)$") {
    throw "Studio package must not bundle runtime Drivers: $name"
  }

  $destination = Join-Path $StageDir $name
  Copy-Item -LiteralPath $binary -Destination $destination
  $copied[$key] = $binary

  foreach ($dependency in Get-Dependents $binary) {
    $resolved = Resolve-Dependency $dependency
    if (-not [string]::IsNullOrWhiteSpace($resolved)) {
      $queue.Enqueue($resolved)
    }
  }
}

$forbidden = Get-ChildItem -LiteralPath $StageDir -File |
  Where-Object {
    $_.Name -match "^(?i:turbodb_driver_.*\.dll)$" -or
    $_.Extension -in @(".lib", ".pdb", ".h", ".hpp", ".c", ".cpp")
  }
if ($forbidden) {
  throw "forbidden development/Driver files entered Studio package: $($forbidden.Name -join ', ')"
}

$manifest = [System.Collections.Generic.List[string]]::new()
$manifest.Add("package=TurboDBStudio")
$manifest.Add("rid=windows-x64")
$manifest.Add("source=$($env:GITHUB_SHA)")
$manifest.Add("drivers=bundled:none")
$manifest.Add("driver_deployment=explicit")
foreach ($file in Get-ChildItem -LiteralPath $StageDir -File | Sort-Object Name) {
  $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $manifest.Add("file=$($file.Name);sha256=$hash")
}
$manifestPath = Join-Path $StageDir "studio-manifest.txt"
$manifest | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ArchivePath -CompressionLevel Optimal

Write-Host "Staged TurboDB Studio runtime closure:"
Get-ChildItem -LiteralPath $StageDir -File |
  Sort-Object Name |
  ForEach-Object { Write-Host "  $($_.Name)" }
Write-Host "Archive: $ArchivePath"
