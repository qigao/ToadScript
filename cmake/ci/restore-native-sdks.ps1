param(
  [Parameter(Mandatory = $true)][string]$Rid,
  [string]$HostRid,
  [string]$Profile,
  [switch]$Local
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath("$PSScriptRoot/../..")
if ($Local) {
  # Load build settings only; credentials remain in the parent environment.
  Get-Content -LiteralPath "$root/.env" | ForEach-Object {
    if ($_ -match '^((?:VCPKG_|SALTS_|SALTSCORE_|CMETA_|CHTTP_|TURBODB_|LLVM_|MSVC_|GCC_|ANDROID_|RE2C_|LEMON_|INSTALL_)[A-Za-z0-9_]*|VSDEVCMD_BAT|CMAKE_BUILD_PARALLEL_LEVEL)=(.*)$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2].Trim('"', "'"))
    }
  }
} elseif (-not $env:GITHUB_ENV) {
  throw 'GITHUB_ENV is required in CI; use -Local for local restoration'
}
if (-not $env:GH_TOKEN -and -not $env:GITHUB_TOKEN) {
  $env:GH_TOKEN = gh auth token
  if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI authentication failed' }
}
if (-not $env:GITHUB_TOKEN) { $env:GITHUB_TOKEN = $env:GH_TOKEN }
$arguments = @("$PSScriptRoot/restore-native-sdks.py", '--rid', $Rid)
if ($HostRid) { $arguments += @('--host-rid', $HostRid) }
if ($Profile) { $arguments += @('--profile', $Profile) }
$output = python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Published SDK restoration failed' }
$values = $output | ConvertFrom-Json -AsHashtable
foreach ($entry in $values.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
  if (-not $Local) { "$($entry.Key)=$($entry.Value)" >> $env:GITHUB_ENV }
}
Write-Host "Restored the selected SDKs for $Rid ($env:VCPKG_TARGET_TRIPLET)"
