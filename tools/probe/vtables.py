"""Finds classes' tables of virtual functions in a copy of a game's exe made
by dumpimage.ps1, from the type information the compiler left in it.

    python tools/probe/vtables.py local/crash.img hknpWorld hknpCharacterProxy
    python tools/probe/vtables.py local/crash.img --like Locomotion

Prints, for each class, where its vtable is as a distance into the exe. An
object of that class in the running game starts with a pointer to exe + that.

How: MSVC writes a type descriptor for every class with virtual functions,
containing its decorated name (".?AVName@@"). A "complete object locator"
points at the descriptor, and the word just before a vtable points at the
locator. In a 64-bit exe the locator's pointers are distances into the exe;
the word before the vtable is a real pointer, as the running game had it,
which is why the copy's load address (<img>.base.txt) is needed.
"""
import re
import struct
import sys


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    path = args[0]
    data = open(path, "rb").read()
    base = int(open(path + ".base.txt").read().strip(), 16)

    # Every type descriptor: name -> where the descriptor starts (16 bytes
    # before its name).
    descriptors = {}
    for m in re.finditer(rb"\.\?A[VU]([A-Za-z_][A-Za-z0-9_]*)@@\x00", data):
        descriptors.setdefault(m.group(1).decode(), m.start() - 16)

    if args[1] == "--like":
        wanted = sorted(n for n in descriptors if any(a.lower() in n.lower() for a in args[2:]))
    else:
        wanted = args[1:]

    targets = {descriptors[n]: n for n in wanted if n in descriptors}
    for n in wanted:
        if n not in descriptors:
            print(f"{n}: no type information in this exe")

    # Complete object locators that name one of those descriptors: signature
    # 1, offset, cdOffset, descriptor, class hierarchy, self. `self` is the
    # locator's own distance into the exe, which is what makes a match sure.
    locators = {}
    for at in range(0, len(data) - 24, 4):
        if data[at:at + 4] != b"\x01\x00\x00\x00":
            continue
        offset, _, descriptor, _, own = struct.unpack_from("<IIIII", data, at + 4)
        if own == at and descriptor in targets:
            locators[base + at] = (targets[descriptor], offset)

    # The pointer to a locator sits just before the vtable it belongs to.
    found = {}
    for at in range(0, len(data) - 8, 8):
        value = struct.unpack_from("<Q", data, at)[0]
        if value in locators:
            name, offset = locators[value]
            found.setdefault(name, []).append((offset, at + 8))

    for name in wanted:
        for offset, vtable in sorted(found.get(name, [])):
            part = "" if offset == 0 else f"  (the part of the object at +0x{offset:X})"
            print(f"{name}: vtable exe+0x{vtable:X}{part}")
        if name in descriptors and name not in found:
            print(f"{name}: type information but no vtable found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
