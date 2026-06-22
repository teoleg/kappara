#!/usr/bin/env python3
"""
tools/pad_pe.py -- pad the kernel image so its file size matches the
PE SizeOfImage field baked into our boot.S header.

AWS.md stage B: objcopy -O binary drops trailing BSS (since the .bss
program header has filesz=0), so the binary file is a bit shorter
than __kernel_end - __kernel_start.  The PE section we declared
covers the full virtual size though -- EDK II's PE loader sees
SizeOfRawData reaching past the file end and refuses to load us
("Script Error Status: Unsupported").  Pad the file with zeros so
the section size and the file size agree.
"""

import struct
import sys

if len(sys.argv) != 2:
    sys.exit("usage: pad_pe.py kernel.img")

path = sys.argv[1]
with open(path, "rb") as f:
    data = bytearray(f.read())

# PE header lives at e_lfanew (DOS bytes 60-63).
if len(data) < 64 or data[:2] != b"MZ":
    sys.exit("pad_pe.py: not a PE file (no MZ at offset 0)")

pe_offset = struct.unpack("<I", data[60:64])[0]
if data[pe_offset:pe_offset + 4] != b"PE\x00\x00":
    sys.exit("pad_pe.py: PE signature missing at offset 0x%x" % pe_offset)

# Optional header starts at pe_offset + 4 (signature) + 20 (COFF).
opt_off = pe_offset + 24
size_of_image = struct.unpack("<I", data[opt_off + 56:opt_off + 60])[0]

old_len = len(data)
if size_of_image > old_len:
    data.extend(b"\x00" * (size_of_image - old_len))
    with open(path, "wb") as f:
        f.write(data)
    print("pad_pe: %d -> %d bytes (+%d)" %
          (old_len, size_of_image, size_of_image - old_len))
else:
    # Nothing to do -- file already at least SizeOfImage.
    pass
