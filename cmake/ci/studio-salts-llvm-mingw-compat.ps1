# Narrow source-level Windows portability patch against exact immutable
# Salts 2.3.0-rc.17. This does not create a second allocator/provider.
# Both MSVC and llvm-mingw must pair _aligned_malloc with _aligned_free;
# only non-Windows platforms may call posix_memalign with free().
param(
  [Parameter(Mandatory = $true)][string]$SourceRoot,
  [Parameter(Mandatory = $true)][string]$ExpectedHead
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$actualHead = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualHead -ne $ExpectedHead -or
    $ExpectedHead -ne '6d6fd387cb245be8d7b0d866f9babad026a26832') {
  throw "Salts CMeta allocator patch only admits immutable v2.3.0-rc.17: $actualHead"
}
$source = Join-Path $root 'cmeta/src/data.c'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
  throw "Missing original Salts CMeta data.c"
}
$utf8 = [Text.UTF8Encoding]::new($false)
$content = [IO.File]::ReadAllText($source, $utf8)
$original = '#if defined(_MSC_VER)'
$matches = [Regex]::Matches($content, [Regex]::Escape($original))
if ($matches.Count -ne 4) {
  throw "Immutable Salts CMeta Windows allocator guard count changed: $($matches.Count)"
}
foreach ($expected in @(
    '#include <malloc.h>',
    'storage = _aligned_malloc(padded, alignment);',
    '_aligned_free(storage);',
    '_aligned_free(temp->storage);',
    'if (posix_memalign(&storage, alignment, padded) != 0) storage = NULL;'
)) {
  if (-not $content.Contains($expected)) {
    throw "Immutable CMeta allocation/cleanup semantic anchor changed: $expected"
  }
}
# Exactly four Windows ownership paths: system malloc.h, allocation, both
# exception/init-failure and close() cleanup sites. Preserve source bytes
# elsewhere and all original overflow/invalid-alignment checks.
$patched = $content.Replace($original, '#if defined(_WIN32)')
[IO.File]::WriteAllText($source, $patched, $utf8)
if ([Regex]::Matches($patched, [Regex]::Escape('#if defined(_WIN32)')).Count -lt 4) {
  throw 'Patched Windows allocator guard verification failed'
}
Write-Host 'STUDIO_SALTS_NATIVE_ALIGN_ADMITTED guards=4 allocator=_aligned_malloc deallocator=_aligned_free'
