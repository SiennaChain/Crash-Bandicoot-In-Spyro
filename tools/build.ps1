# Builds both bridges, the monitor and the protocol tests, and runs the tests.
#
#   tools\build.ps1            Release build into build\out\
#   tools\build.ps1 -Clean     from scratch
#
# Needs Visual Studio with the desktop C++ workload. Its bundled CMake is
# used, so nothing has to be on PATH.
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio was not found (no vswhere.exe).' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ tools was found.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "Visual Studio's CMake was not found at $cmake" }

if ($Clean -and (Test-Path $build)) { [IO.Directory]::Delete($build, $true) }

& $cmake -S $root -B $build -A x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $cmake --build $build --config Release -- /nologo /verbosity:minimal | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$tests = Get-ChildItem $build -Recurse -Filter csp_tests.exe | Select-Object -First 1
if (-not $tests) { throw 'csp_tests.exe was not built' }
& $tests.FullName | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'protocol tests failed' }

Get-ChildItem (Join-Path $build 'out') -Recurse -Include *.dll, *.exe |
  Select-Object @{n = 'KB'; e = { [math]::Round($_.Length / 1KB) } }, @{n = 'File'; e = { $_.FullName.Substring($root.Length + 1) } } |
  Format-Table -AutoSize | Out-Host
