# Looks for a way from one object in a running game to another by following
# pointers: which field of A points at B, or at something that points at B,
# up to three steps. For finding how to reach a thing from something already
# known. Read-only.
#
#   tools\probe\findpath.ps1 -Process CrashBandicootNSaneTrilogy -From 0x24F95701A20 -To 0x25018658120
#
# -From may also be given as a place in the exe holding a pointer:
#   -From 'exe+0x1A9E778'  (the pointer stored there is the starting object)
param([Parameter(Mandatory)][string]$Process, [Parameter(Mandatory)][string]$From, [Parameter(Mandatory)][string]$To,
      [int]$Depth = 3, [string]$Span = '0x800')

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class PathFind {
  [DllImport("kernel32", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  public static byte[] Read(IntPtr process, long address, int size) {
    byte[] buffer = new byte[size]; IntPtr read;
    if (address < 0x10000 || !ReadProcessMemory(process, new IntPtr(address), buffer, new IntPtr(size), out read)) return null;
    if (read.ToInt64() != size) Array.Resize(ref buffer, (int)read.ToInt64());
    return buffer;
  }
  // Breadth first: each object's first `span` bytes are searched for pointers.
  public static List<string> Find(IntPtr process, long from, long to, int depth, int span, long imageLow, long imageHigh) {
    var found = new List<string>();
    var seen = new HashSet<long>();
    var level = new List<KeyValuePair<long, string>> { new KeyValuePair<long, string>(from, "") };
    seen.Add(from);
    for (int d = 0; d < depth && level.Count > 0 && found.Count < 12; d++) {
      var next = new List<KeyValuePair<long, string>>();
      foreach (var item in level) {
        byte[] data = Read(process, item.Key, span);
        if (data == null) continue;
        for (int i = 0; i + 8 <= data.Length; i += 8) {
          long value = BitConverter.ToInt64(data, i);
          string path = item.Value + " +0x" + i.ToString("X") + " ->";
          if (value == to) { found.Add(path.TrimEnd(' ', '-', '>')); continue; }
          // Only pointers to heap objects are followed: aligned, in user space, not into the exe.
          if (value < 0x10000 || value > 0x7FFFFFFFFFFF || (value & 7) != 0 || (value >= imageLow && value < imageHigh)) continue;
          if (seen.Add(value) && next.Count < 60000) next.Add(new KeyValuePair<long, string>(value, path));
        }
      }
      level = next;
    }
    return found;
  }
}
'@
$p = @(Get-Process $Process -ErrorAction SilentlyContinue)
if ($p.Count -ne 1) { throw "expected exactly one running '$Process', found $($p.Count)" }
$base = $p[0].MainModule.BaseAddress.ToInt64(); $size = $p[0].MainModule.ModuleMemorySize
$handle = [PathFind]::OpenProcess(0x0410, $false, $p[0].Id)
$start = if ($From -match '^exe\+0x([0-9A-Fa-f]+)$') {
  $slot = $base + [Convert]::ToInt64($Matches[1], 16)
  [BitConverter]::ToInt64([PathFind]::Read($handle, $slot, 8), 0)
} else { [Convert]::ToInt64($From, 16) }
$target = [Convert]::ToInt64($To, 16)
"from 0x{0:X} to 0x{1:X}, up to {2} steps, first {3} bytes of each object" -f $start, $target, $Depth, $Span
$paths = [PathFind]::Find($handle, $start, $target, $Depth, [Convert]::ToInt32($Span, 16), $base, $base + $size)
if ($paths.Count) { $paths | ForEach-Object { "  start$_" } } else { '  no path found' }
