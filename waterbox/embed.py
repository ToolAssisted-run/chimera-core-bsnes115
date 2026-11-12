#!/usr/bin/env python3
"""Writes a file's bytes as a C array: embed.py <input> <output.h> <name>.

The guest has no folder to read upstream's game database from, so the build
compiles it in."""
import sys

src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(out, "w") as f:
    f.write(f"/* generated from {src.split('/')[-1]} by waterbox/embed.py */\n")
    f.write(f"static const unsigned char {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 24):
        f.write(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("};\n")
