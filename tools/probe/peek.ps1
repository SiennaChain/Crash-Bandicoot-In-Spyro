# Prints a stretch of a running game's memory eight bytes at a time, each
# word read three ways: as a pointer (and what it points at, if that starts
# with a pointer into the exe, i.e. is probably an object, with its class
# where the exe carries type information), as two floats, and as two ints.
#
#   tools\probe\peek.ps1 -Process CrashBandicootNSaneTrilogy -Address 0x1F9006EC520 -Bytes 0x140
#
# Read-only. For finding out what an object's fields are.
param([Parameter(Mandatory)][string]$Process, [Parameter(Mandatory)][string]$Address, [string]$Bytes = '0x100')

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Peek {
  [DllImport("kernel32", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  public static byte[] Read(IntPtr process, long address, int size) {
    byte[] buffer = new byte[size]; IntPtr read;
    if (address < 0x10000 || !ReadProcessMemory(process, new IntPtr(address), buffer, new IntPtr(size), out read) || read.ToInt64() != size) return null;
    return buffer;
  }
  // The class of the object at `address`, from the type information beside its vtable; "" if there is none.
  public static string ClassOf(IntPtr process, long address, long imageBase, long imageSize) {
    byte[] first = Read(process, address, 8); if (first == null) return null;
    long vtable = BitConverter.ToInt64(first, 0);
    if (vtable < imageBase || vtable >= imageBase + imageSize) return null;
    string at = "exe+0x" + (vtable - imageBase).ToString("X");
    byte[] before = Read(process, vtable - 8, 8); if (before == null) return at;
    long locator = BitConverter.ToInt64(before, 0);
    if (locator < imageBase || locator >= imageBase + imageSize) return at;
    byte[] l = Read(process, locator, 24); if (l == null || BitConverter.ToInt32(l, 0) != 1) return at;
    byte[] name = Read(process, imageBase + BitConverter.ToUInt32(l, 12) + 16, 96); if (name == null) return at;
    int end = Array.IndexOf(name, (byte)0); string text = Encoding.ASCII.GetString(name, 0, end < 0 ? name.Length : end);
    return text.StartsWith(".?A") ? text.Substring(4).TrimEnd('@') + " (" + at + ")" : at;
  }
}
'@
$found = @(Get-Process $Process -ErrorAction SilentlyContinue)
if ($found.Count -ne 1) { throw "expected exactly one running '$Process', found $($found.Count)" }
$imageBase = $found[0].MainModule.BaseAddress.ToInt64(); $imageSize = $found[0].MainModule.ModuleMemorySize
$handle = [Peek]::OpenProcess(0x0410, $false, $found[0].Id)
$start = [Convert]::ToInt64($Address, 16); $count = [Convert]::ToInt32($Bytes, 16)
$data = [Peek]::Read($handle, $start, $count)
if (-not $data) { throw "cannot read $count bytes at $Address" }
"{0} is a {1}" -f $Address, [Peek]::ClassOf($handle, $start, $imageBase, $imageSize)
for ($i = 0; $i + 8 -le $count; $i += 8) {
  $q = [BitConverter]::ToInt64($data, $i)
  $what = ''
  if ($q -ge $imageBase -and $q -lt $imageBase + $imageSize) { $what = 'exe+0x{0:X}' -f ($q - $imageBase) }
  elseif ($q -gt 0x10000 -and $q -lt 0x7FFFFFFFFFFF -and ($q -band 7) -eq 0) { $c = [Peek]::ClassOf($handle, $q, $imageBase, $imageSize); if ($c) { $what = "-> $c" } elseif ([Peek]::Read($handle, $q, 8)) { $what = '-> memory' } }
  "+0x{0:X3}  {1:X16}  {2,14:G6} {3,14:G6}  {4,11} {5,11}  {6}" -f $i, $q, [BitConverter]::ToSingle($data, $i), [BitConverter]::ToSingle($data, $i + 4), [BitConverter]::ToInt32($data, $i), [BitConverter]::ToInt32($data, $i + 4), $what
}
