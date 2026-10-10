# Counts the objects of given classes alive in a running game, by looking
# through its memory for pointers to those classes' vtables (which is what
# every object of a class with virtual functions starts with).
#
#   tools\probe\findobjects.ps1 -Process CrashBandicootNSaneTrilogy `
#       -Classes 'hknpWorld=0x1854058', 'hknpCharacterProxy=0x1856DB8'
#
# The numbers after the names are vtable distances into the exe, as
# tools\probe\vtables.py prints them. Read-only: the game is opened for
# query and read. A match is only "something that starts with that pointer";
# a copy of an object or a stale one counts too, so treat small counts as
# real and large ones as an upper bound.
param([Parameter(Mandatory)][string]$Process, [Parameter(Mandatory)][string[]]$Classes, [int]$Show = 4)

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ObjectScan {
  [StructLayout(LayoutKind.Sequential)]
  struct MBI { public IntPtr BaseAddress, AllocationBase; public uint AllocationProtect; public ushort Partition, pad; public IntPtr RegionSize; public uint State, Protect, Type; }
  [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32")] static extern IntPtr VirtualQueryEx(IntPtr process, IntPtr address, out MBI info, IntPtr size);
  [DllImport("kernel32")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  [DllImport("kernel32")] static extern bool CloseHandle(IntPtr handle);

  public static Dictionary<long, List<long>> Find(int pid, long[] wanted, out long scanned) {
    var found = new Dictionary<long, List<long>>();
    foreach (long w in wanted) found[w] = new List<long>();
    scanned = 0;
    IntPtr process = OpenProcess(0x0410, false, pid);
    if (process == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
    try {
      byte[] buffer = new byte[1 << 20];
      long address = 0x10000;
      MBI info;
      while (address < 0x7FFFFFFF0000 && VirtualQueryEx(process, new IntPtr(address), out info, new IntPtr(Marshal.SizeOf(typeof(MBI)))) != IntPtr.Zero) {
        long start = info.BaseAddress.ToInt64(), size = info.RegionSize.ToInt64();
        // Committed, private, readable and writable, not a guard page: the heaps.
        bool heap = info.State == 0x1000 && info.Type == 0x20000 && (info.Protect & 0x04) != 0 && (info.Protect & 0x100) == 0;
        if (heap) {
          for (long offset = 0; offset < size; offset += buffer.Length) {
            int want = (int)Math.Min(buffer.Length, size - offset);
            IntPtr read;
            if (!ReadProcessMemory(process, new IntPtr(start + offset), buffer, new IntPtr(want), out read)) continue;
            int got = (int)read.ToInt64();
            scanned += got;
            for (int i = 0; i + 8 <= got; i += 8) {
              long value = BitConverter.ToInt64(buffer, i);
              List<long> list;
              if (value != 0 && found.TryGetValue(value, out list)) list.Add(start + offset + i);
            }
          }
        }
        address = start + size;
      }
    } finally { CloseHandle(process); }
    return found;
  }
}
'@

$found = @(Get-Process $Process -ErrorAction SilentlyContinue)
if ($found.Count -ne 1) { throw "expected exactly one running '$Process', found $($found.Count)" }
$base = $found[0].MainModule.BaseAddress.ToInt64()
$names = @{}
# 'name=0x1234' is a vtable, a distance into the exe; 'name=@0x1F900344900' is
# any other value to look for as it stands, e.g. the address of one object, to
# find everything that points at it.
$wanted = foreach ($c in $Classes) {
  $name, $value = $c -split '='
  $v = if ($value.StartsWith('@')) { [Convert]::ToInt64($value.Substring(1), 16) } else { $base + [Convert]::ToInt64($value, 16) }
  $names[$v] = $name; $v
}
$scanned = 0
$result = [ObjectScan]::Find($found[0].Id, [long[]]$wanted, [ref]$scanned)
"{0}: looked through {1:N0} MB" -f $Process, ($scanned / 1MB)
foreach ($v in $wanted) {
  $list = $result[$v]
  "{0,-34} {1,6}   {2}" -f $names[$v], $list.Count, (($list | Select-Object -First $Show | ForEach-Object { '0x{0:X}' -f $_ }) -join '  ')
}
