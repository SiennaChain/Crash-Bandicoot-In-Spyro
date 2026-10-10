"""Finds the code in a copy of a game's exe (made by dumpimage.ps1) that
refers to given places in it: a vtable, a global, a routine.

    python tools/probe/xrefs.py local/crash.img 0x1856538 [0x... ...]
    python tools/probe/xrefs.py local/crash.img --calls 0x14F170

64-bit code names a place in its own exe by how far it is from the end of
the instruction doing the naming. So every four bytes of code are tried as
such a distance, for the three places an instruction's end can be relative
to it (straight after, or after one or four more bytes of operand), and the
ones that land exactly on a wanted place are reported. Chance hits are about
one in two hundred runs per place.

With --calls only `call` and `jmp` instructions to the place are reported.
Prints distances into the exe, the form everything else here uses.
"""
import struct
import sys

import numpy as np


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(count):
        entry = table + i * 40
        name = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
        size, address = struct.unpack_from("<II", data, entry + 8)
        yield name, address, size


def main():
    args = sys.argv[1:]
    calls = "--calls" in args
    args = [a for a in args if a != "--calls"]
    if len(args) < 2:
        print(__doc__)
        return 1
    data = open(args[0], "rb").read()
    targets = [int(a, 16) for a in args[1:]]
    text = next((address, size) for name, address, size in sections(data) if name == ".text")
    start, size = text
    code = np.frombuffer(data, dtype=np.uint8, count=size, offset=start).astype(np.int64)
    # every four bytes as a signed distance
    distance = code[:-3] | (code[1:-2] << 8) | (code[2:-1] << 16) | (code[3:] << 24)
    distance = np.where(distance >= 1 << 31, distance - (1 << 32), distance)
    at = np.arange(len(distance), dtype=np.int64) + start  # where each distance sits

    for target in targets:
        found = []
        for extra in (0,) if calls else (0, 1, 4):
            for where in at[(at + 4 + extra + distance) == target]:
                where = int(where)
                opcode = data[where - 1]
                if calls:
                    if opcode not in (0xE8, 0xE9):
                        continue
                    kind = "call" if opcode == 0xE8 else "jmp"
                    found.append((where - 1, kind))
                else:
                    kind = {0: "", 1: " (one more byte of operand follows)", 4: " (four more bytes of operand follow)"}[extra]
                    found.append((where, "distance at this place" + kind))
        print(f"exe+0x{target:X}: {len(found)} reference(s)")
        for where, kind in sorted(found):
            print(f"  exe+0x{where:X}  {kind}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
