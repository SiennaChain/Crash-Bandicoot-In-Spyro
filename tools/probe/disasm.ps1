# Disassembles a stretch of a copy of a game's exe (made by dumpimage.ps1),
# with every address shown as a distance into the exe.
#
#   tools\probe\disasm.ps1 -Image local\crash.img -Rva 0x169B90 -Bytes 0x80
#
# Needs Visual Studio's dumpbin.
param([Parameter(Mandatory)][string]$Image, [Parameter(Mandatory)][string]$Rva, [string]$Bytes = '0x100', [switch]$WithBytes)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe | Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
$path = (Resolve-Path $Image).Path
$base = [Convert]::ToInt64((Get-Content "$path.base.txt").Trim(), 16)
$from = $base + [Convert]::ToInt64($Rva, 16)
$to = $from + [Convert]::ToInt64($Bytes, 16)
$mode = if ($WithBytes) { '/disasm' } else { '/disasm:nobytes' }
& $dumpbin.FullName /nologo $mode ("/range:0x{0:X},0x{1:X}" -f $from, $to) $path | Where-Object { $_ -match '^\s+[0-9A-F]{16}:' } | ForEach-Object {
  $line = $_
  foreach ($m in [regex]::Matches($line, '0000([0-9A-F]{12})h?')) {
    $value = [Convert]::ToInt64($m.Groups[1].Value, 16)
    if ($value -ge $base -and $value -lt $base + 0x10000000) { $line = $line.Replace($m.Value, ('exe+0x{0:X}' -f ($value - $base))) }
  }
  $line.Trim()
}
