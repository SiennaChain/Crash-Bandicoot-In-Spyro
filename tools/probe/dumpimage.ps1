# Copies a running game's exe out of its memory into a file that dumpbin can
# disassemble. Both exes are stored encrypted (Steam's wrapper) and only exist
# in the clear while the game runs, so this is the only way to read their code.
#
#   tools\probe\dumpimage.ps1 -Process CrashBandicootNSaneTrilogy -Out local\crash.img
#   tools\probe\dumpimage.ps1 -Process Spyro-Win64-Shipping       -Out local\spyro.img
#
# Read-only: the game is opened for query and read, nothing else. The copy has
# its section table rewritten so that an offset in the file is the same as a
# distance into the exe, and says where the exe was loaded in <Out>.base.txt,
# because pointers inside the copy are as the running game had them.
#
# The copy is the game's code: keep it under local\ (ignored by git), never
# commit it.
param([Parameter(Mandatory)][string]$Process, [Parameter(Mandatory)][string]$Out)

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class ImageDump {
  [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32", SetLastError = true)] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
  [DllImport("kernel32")] static extern bool CloseHandle(IntPtr handle);

  // Returns the number of bytes that could not be read (left as zeros).
  public static long Dump(int pid, long imageBase, int imageSize, string path) {
    IntPtr process = OpenProcess(0x0410, false, pid);  // query information + read memory
    if (process == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
    try {
      byte[] image = new byte[imageSize];
      byte[] page = new byte[0x1000];
      long missing = 0;
      for (int offset = 0; offset < imageSize; offset += page.Length) {
        IntPtr read;
        int want = Math.Min(page.Length, imageSize - offset);
        if (ReadProcessMemory(process, new IntPtr(imageBase + offset), page, new IntPtr(want), out read) && read.ToInt64() == want) {
          Buffer.BlockCopy(page, 0, image, offset, want);
        } else {
          missing += want;
        }
      }

      // Make the file read the way memory is laid out: every section's data
      // is where its address says, and nothing points at tables that the
      // wrapper has moved or that only make sense on disk.
      int pe = BitConverter.ToInt32(image, 0x3C);
      int sections = BitConverter.ToUInt16(image, pe + 6);
      int optionalSize = BitConverter.ToUInt16(image, pe + 20);
      int optional = pe + 24;
      int directories = optional + 112;  // PE32+: the 16 data directories
      Array.Clear(image, directories, 16 * 8);
      int table = optional + optionalSize;
      for (int i = 0; i < sections; ++i) {
        int entry = table + i * 40;
        uint virtualSize = BitConverter.ToUInt32(image, entry + 8);
        uint virtualAddress = BitConverter.ToUInt32(image, entry + 12);
        Buffer.BlockCopy(BitConverter.GetBytes(virtualSize), 0, image, entry + 16, 4);     // SizeOfRawData
        Buffer.BlockCopy(BitConverter.GetBytes(virtualAddress), 0, image, entry + 20, 4);  // PointerToRawData
      }
      // FileAlignment = SectionAlignment, or the offsets above are not legal.
      Buffer.BlockCopy(image, optional + 32, image, optional + 36, 4);
      File.WriteAllBytes(path, image);
      return missing;
    } finally {
      CloseHandle(process);
    }
  }
}
'@

$found = @(Get-Process $Process -ErrorAction SilentlyContinue)
if ($found.Count -ne 1) { throw "expected exactly one running '$Process', found $($found.Count)" }
$module = $found[0].MainModule
$path = [IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
[void][IO.Directory]::CreateDirectory((Split-Path $path -Parent))
$missing = [ImageDump]::Dump($found[0].Id, $module.BaseAddress.ToInt64(), $module.ModuleMemorySize, $path)
$base = '0x{0:X}' -f $module.BaseAddress.ToInt64()
[IO.File]::WriteAllText("$path.base.txt", "$base`r`n")
"{0}: pid {1}, loaded at {2}, {3:N0} bytes -> {4} ({5:N0} bytes unreadable)" -f $Process, $found[0].Id, $base, $module.ModuleMemorySize, $path, $missing
