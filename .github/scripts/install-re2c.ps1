$ErrorActionPreference = 'Stop'

$version = '4.6'
$archiveSha256 = '75bf2696445e831d0d44e0d9f2909eeffc18c09757f222b2fb025f7e59fe130b'
$workspace = $env:GITHUB_WORKSPACE
if ([string]::IsNullOrWhiteSpace($workspace)) { throw 'GITHUB_WORKSPACE is required' }

$prefix = Join-Path $workspace 'external/tools/re2c'
$work = Join-Path $env:RUNNER_TEMP 'turbodb-re2c-build'
$archive = Join-Path $env:RUNNER_TEMP "re2c-$version.tar.xz"
$source = Join-Path $work "re2c-$version"
$build = Join-Path $work 'build'

if (Test-Path -LiteralPath $work) { Remove-Item -Recurse -Force -LiteralPath $work }
if (Test-Path -LiteralPath $prefix) { Remove-Item -Recurse -Force -LiteralPath $prefix }
New-Item -ItemType Directory -Force -Path $work | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $prefix) | Out-Null

$curl = if ($IsWindows) { 'curl.exe' } else { 'curl' }
& $curl --disable --fail --location --silent --show-error --connect-timeout 30 --max-time 180 --proto '=https' --proto-redir '=https' "https://github.com/skvadrik/re2c/releases/download/$version/re2c-$version.tar.xz" --output $archive
if ($LASTEXITCODE -ne 0) { throw 're2c download failed' }

$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actual -cne $archiveSha256) { throw "re2c archive checksum mismatch: $actual" }

& cmake -E chdir $work cmake -E tar xJf $archive
if ($LASTEXITCODE -ne 0) { throw 're2c extraction failed' }
if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) {
  throw 're2c source archive is incomplete'
}

$backendOptions = @(
  '-DRE2C_BUILD_RE2D=OFF',
  '-DRE2C_BUILD_RE2GO=OFF',
  '-DRE2C_BUILD_RE2HS=OFF',
  '-DRE2C_BUILD_RE2JAVA=OFF',
  '-DRE2C_BUILD_RE2JS=OFF',
  '-DRE2C_BUILD_RE2OCAML=OFF',
  '-DRE2C_BUILD_RE2PY=OFF',
  '-DRE2C_BUILD_RE2RUST=OFF',
  '-DRE2C_BUILD_RE2SWIFT=OFF',
  '-DRE2C_BUILD_RE2V=OFF',
  '-DRE2C_BUILD_RE2ZIG=OFF'
)
$configure = @(
  '-S', $source,
  '-B', $build,
  '-G', 'Ninja',
  '-DCMAKE_BUILD_TYPE=Release',
  "-DCMAKE_INSTALL_PREFIX=$prefix",
  '-DRE2C_BUILD_TESTS=OFF',
  '-DRE2C_REBUILD_LEXERS=OFF',
  '-DRE2C_REBUILD_PARSERS=OFF',
  '-DRE2C_REBUILD_DOCS=OFF'
) + $backendOptions
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 're2c configure failed' }

& cmake --build $build --parallel 2
if ($LASTEXITCODE -ne 0) { throw 're2c build failed' }
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 're2c install failed' }

$suffix = if ($IsWindows) { '.exe' } else { '' }
$exe = Join-Path $prefix ("bin/re2c" + $suffix)
if (-not (Test-Path -LiteralPath $exe)) { throw 're2c executable is missing' }
$reported = (& $exe --version).Trim()
if ($reported -cne "re2c $version") { throw "unexpected re2c version: $reported" }

foreach ($file in @('share/re2c/stdlib/unicode_properties.re', 'share/re2c/stdlib/unicode_categories.re')) {
  if (-not (Test-Path -LiteralPath (Join-Path $prefix $file))) {
    throw "re2c standard library is missing: $file"
  }
}
Write-Output $reported
