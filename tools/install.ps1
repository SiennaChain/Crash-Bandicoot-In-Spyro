# Puts the two bridges into the game folders, or takes them out again.
#
#   tools\install.ps1              copy the built DLLs (and an ini, if there is none yet)
#   tools\install.ps1 -Uninstall   remove the DLLs, inis and logs
#   tools\install.ps1 -WhatIf      say what would be done, and do nothing
#   tools\install.ps1 -Only crash  just one game's (crash or spyro), e.g. while the other is running
#
# The games are found through Steam. Nothing of either game is changed: each
# bridge is one extra DLL beside the exe, named after the XInput DLL the game
# would otherwise load from Windows. Run tools\build.ps1 first.
[CmdletBinding(SupportsShouldProcess)]
param([switch]$Uninstall, [ValidateSet('crash', 'spyro')][string[]]$Only)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$marker = 'CrashSpyroBridgeMarker'

function Find-SteamGame([string]$appId) {
  $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
  if (-not $steam) { throw 'Steam was not found.' }
  $libraries = @($steam)
  $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
  if (Test-Path $vdf) {
    Get-Content $vdf | ForEach-Object {
      if ($_ -match '"path"\s+"(.*)"') { $libraries += ($Matches[1] -replace '\\\\', '\') }
    }
  }
  foreach ($library in $libraries | Select-Object -Unique) {
    $manifest = Join-Path $library "steamapps\appmanifest_$appId.acf"
    if (Test-Path $manifest) {
      if ((Get-Content $manifest -Raw) -match '"installdir"\s+"([^"]*)"') {
        return Join-Path $library "steamapps\common\$($Matches[1])"
      }
    }
  }
  throw "Steam app $appId is not installed."
}

# Whether a DLL already in a game folder is one of ours, so that somebody
# else's file of the same name is never overwritten or deleted.
function Test-Ours([string]$path) {
  $bytes = [IO.File]::ReadAllBytes($path)
  $text = [Text.Encoding]::ASCII.GetString($bytes)
  return $text.Contains($marker)
}

$games = @(
  @{ Name = 'Crash'; Exe = 'CrashBandicootNSaneTrilogy'; Dll = 'xinput9_1_0.dll'; Source = 'crash'
     Folder = (Find-SteamGame '731490') },
  @{ Name = 'Spyro'; Exe = 'Spyro-Win64-Shipping'; Dll = 'xinput1_3.dll'; Source = 'spyro'
     Folder = (Join-Path (Find-SteamGame '996580') 'Falcon\Binaries\Win64') }
)

if ($Only) { $games = @($games | Where-Object { $Only -contains $_.Source }) }

foreach ($game in $games) {
  if (Get-Process $game.Exe -ErrorAction SilentlyContinue) {
    throw "$($game.Name) is running. Close it first: a game holds its DLLs open."
  }
  if (-not (Test-Path (Join-Path $game.Folder "$($game.Exe).exe"))) {
    throw "$($game.Name)'s exe was not found in $($game.Folder)"
  }
}

foreach ($game in $games) {
  $dll = Join-Path $game.Folder $game.Dll
  $ini = Join-Path $game.Folder 'crashspyro.ini'
  if ((Test-Path $dll) -and -not (Test-Ours $dll)) {
    throw "$dll is not one of this mod's files. Leaving $($game.Name) alone."
  }

  if ($Uninstall) {
    foreach ($file in $dll, $ini, (Join-Path $game.Folder "crashspyro_$($game.Source).log")) {
      if ((Test-Path $file) -and $PSCmdlet.ShouldProcess($file, 'remove')) { [IO.File]::Delete($file) }
    }
    if (-not $WhatIfPreference) { Write-Host "$($game.Name): removed from $($game.Folder)" }
    continue
  }

  $built = Join-Path $root "build\out\$($game.Source)\$($game.Dll)"
  if (-not (Test-Path $built)) { throw "$built is missing. Run tools\build.ps1 first." }
  if ($PSCmdlet.ShouldProcess($dll, 'install')) { [IO.File]::Copy($built, $dll, $true) }
  # An ini that is already there holds whatever is being worked on; keep it.
  if (-not (Test-Path $ini)) {
    $example = Join-Path $root "$($game.Source)\crashspyro.ini.example"
    if ($PSCmdlet.ShouldProcess($ini, 'install')) { [IO.File]::Copy($example, $ini, $false) }
  }
  if (-not $WhatIfPreference) { Write-Host "$($game.Name): installed in $($game.Folder)" }
}
