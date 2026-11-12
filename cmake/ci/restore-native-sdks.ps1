param(
  [Parameter(Mandatory = $true)][string]$Rid,
  [string]$HostRid,
  [string]$Profile,
  [switch]$Local
)
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath("$PSScriptRoot/../..")
if ($Local) {
  Get-Content "$root/.env" | ForEach-Object {
    if ($_ -match '^((?:VCPKG_|SALTS_|SALTSCORE_|CMETA_|LLVM_|MSVC_|GCC_|ANDROID_|RE2C_)[A-Za-z0-9_]*|VSDEVCMD_BAT)=(.*)$') {
      [Environment]::SetEnvironmentVariable($matches[1], $matches[2].Trim('"', "'"))
    }
  }
}
$env:GH_TOKEN = gh auth token
if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI authentication failed' }
$arguments = @("$PSScriptRoot/restore-native-sdks.py", '--rid', $Rid)
if ($HostRid) { $arguments += @('--host-rid', $HostRid) }
if ($Profile) { $arguments += @('--profile', $Profile) }
python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Released SDK restoration failed' }
$values = Get-Content "$root/build/native-sdk-environment.json" -Raw | ConvertFrom-Json -AsHashtable
foreach ($entry in $values.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
Write-Host "Restored SaltsBase $env:SALTS_BASE_VERSION, CMeta $env:CMETA_VERSION, SaltsUtils $env:SALTS_UTILS_VERSION and CNet $env:SALTS_CORE_VERSION"
