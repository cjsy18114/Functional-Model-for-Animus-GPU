"""Independent Fraction UV/texel oracle for the machine-code FS fixture.

The driver only supplies context and memory. The shader loads UV bits, executes
gfx.tex and exports via gfx.end. No graphics pipeline/rasterization is implied.
"""
from fractions import Fraction
import json
from pathlib import Path
import random
import struct
import subprocess
import sys


def coordinate(bits, extent):
    magnitude = bits & 0x7fffffff
    if magnitude > 0x7f800000 or bits >> 31:
        return 0
    if magnitude >= 0x3f800000:
        return extent - 1
    exponent = magnitude >> 23
    mantissa = magnitude & 0x7fffff
    value = Fraction(mantissa, 1 << 149) if exponent == 0 else Fraction((1 << 23) + mantissa) * Fraction(2) ** (exponent - 150)
    return min((value * extent).__floor__(), extent - 1)


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else "build/animus_graphics_demo.exe").resolve()
    colors = [0xff0000ff,0xff00ff00,0xffff0000,0xffffffff,0xff000000,0xffff00ff]
    boundaries = [0,0x80000000,1,0x3eaaaaaa,0x3eaaaaab,0x3f000000,0x3f2aaaaa,0x3f2aaaab,
                  0x3f7fffff,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc01234,0xff800001]
    pairs = [(u,v) for u in boundaries for v in boundaries]
    rng = random.Random(20261107)
    while len(pairs) < 1024:
        pairs.append((rng.getrandbits(32),rng.getrandbits(32)))
    path = Path("build/texture_oracle_uv.bin")
    path.parent.mkdir(exist_ok=True)
    for start in range(0,len(pairs),32):
        batch = pairs[start:start+32]
        path.write_bytes(b"".join(struct.pack("<II",u,v) for u,v in batch))
        result = subprocess.run([str(executable),"cases/fs_texture.bin",str(path)],capture_output=True,text=True,check=True)
        rows = [json.loads(row) for row in result.stdout.splitlines()]
        assert len(rows) == 8 and rows[-1]["op"] == "gfx.end" and rows[-1]["ended"]
        outputs = rows[-1]["outputs"]
        assert outputs["stage"] == "fs" and not outputs["vertices"]
        fragments = outputs["fragments"]
        assert len(fragments) == len(batch)
        for lane,((u,v),fragment) in enumerate(zip(batch,fragments)):
            expected = colors[coordinate(v,2)*3+coordinate(u,3)]
            assert fragment["lane"] == lane and fragment["pixel_x"] == lane
            assert fragment["rgba8"] == expected, (hex(u),hex(v),hex(expected),fragment)
            assert fragment["pixel_y"] == 7 and fragment["depth_bits"] == 0x3f000000
    print(f"PASS texture Fraction oracle: {len(pairs)} UV pairs, machine-code FS, packed color and output identity")


if __name__ == "__main__":
    main()
