# Starts a session: both games, each with its bridge in, Crash going straight
# into the level he only has to stand in. There is no launcher yet; this is
# what stands in for one.
#
#   tools\run.ps1                 close both games if they are running, then start both
#   tools\run.ps1 -Install        put the built bridges in first (tools\build.ps1 builds them)
#   tools\run.ps1 -Level crash3/l300_hub/l300_hub
#                                 start Crash in another level: any the game has, by the
#                                 name its -om switch takes (docs/PHASE1.md, "A flat world
#                                 for Crash to be in")
#
# Both games are always closed first: the two bridges share a block of memory
# whose layout is fixed by whichever starts first, so a game left running
# from before a rebuild would not understand the other.
#
# Then, in Spyro: F2 puts Crash where Spyro is, and F2 again lets go.
param([string]$Level = 'crash1/custom_level/custom_level', [switch]$Install)

$ErrorActionPreference = 'Stop'

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

$steamExe = Join-Path ((Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath -replace '/', '\') 'steam.exe'
if (-not (Test-Path $steamExe)) { throw "steam.exe was not found at $steamExe" }
$crashFolder = Find-SteamGame '731490'
$spyroFolder = Join-Path (Find-SteamGame '996580') 'Falcon\Binaries\Win64'

# Asked to close, as if by their own close buttons, and given time to.
foreach ($name in 'Spyro-Win64-Shipping', 'CrashBandicootNSaneTrilogy') {
  foreach ($process in @(Get-Process $name -ErrorAction SilentlyContinue)) {
    [void]$process.CloseMainWindow()
    if (-not $process.WaitForExit(30000)) { throw "$name did not close when asked. Close it by hand and run this again." }
  }
}

if ($Install) { & (Join-Path $PSScriptRoot 'install.ps1') }
foreach ($bridge in (Join-Path $crashFolder 'xinput9_1_0.dll'), (Join-Path $spyroFolder 'xinput1_3.dll')) {
  if (-not (Test-Path $bridge)) { throw "$bridge is not there. Run tools\build.ps1 and then this with -Install." }
}

# The empty level is not the game's own: it has to have been put in (the
# level editor's Play does it). Without it, the game's own crate room is the
# next best place to stand.
if ($Level -eq 'crash1/custom_level/custom_level' -and -not (Test-Path (Join-Path $crashFolder 'archives\Custom_Level.pak'))) {
  Write-Host 'The empty level is not installed in Crash (docs/PHASE1.md, "The empty level"); using the game''s crate room instead.'
  $Level = 'whiterooms/wr_crates/crash_crates/crash_crates'
}

$crashLog = Join-Path $crashFolder 'crashspyro_crash.log'
$spyroLog = Join-Path $spyroFolder 'crashspyro_spyro.log'
# The last session's logs would be read as this one's below.
foreach ($log in $crashLog, $spyroLog) { if (Test-Path $log) { [IO.File]::Delete($log) } }

Write-Host 'Starting Spyro ...'
& $steamExe -applaunch 996580
Start-Sleep -Seconds 6
Write-Host "Starting Crash in $Level ..."
& $steamExe -applaunch 731490 -om $Level

# What each bridge says once it is up, so that a start that went wrong shows.
$deadline = (Get-Date).AddSeconds(120)
$crashUp = $false; $spyroUp = $false
while ((Get-Date) -lt $deadline -and -not ($crashUp -and $spyroUp)) {
  Start-Sleep -Seconds 2
  $crashUp = (Test-Path $crashLog) -and [bool](Select-String -Path $crashLog -Pattern 'player yes' -Quiet)
  $spyroUp = (Test-Path $spyroLog) -and [bool](Select-String -Path $spyroLog -Pattern 'pawn yes' -Quiet)
}
Write-Host ("Crash: " + $(if ($crashUp) { 'in his level' } else { "not in a level yet (see $crashLog)" }))
Write-Host ("Spyro: " + $(if ($spyroUp) { 'in a level' } else { "not in a level yet: take it to one (see $spyroLog)" }))
if ($crashUp -and $spyroUp) { Write-Host 'Press F2 in Spyro to put Crash there; F2 again lets go.' }
