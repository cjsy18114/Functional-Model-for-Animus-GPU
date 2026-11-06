"""Optional independent Python-integer oracle for B1/B7 (no third-party packages).

Run from the repository root after building. The C++ decoder/executor does not
share arithmetic helpers with this oracle. Generated artifacts stay in build/.
"""
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

MASK = (1 << 32) - 1


def signed(value):
    return value if value < (1 << 31) else value - (1 << 32)


def quotient(a, b):
    if b == 0:
        return -1
    magnitude = abs(a) // abs(b)
    return -magnitude if (a < 0) != (b < 0) else magnitude


def initialize(reg, value):
    # RISC-V LUI + ADDI reconstruct any 32-bit integer without executing a load.
    high = (value + 0x800) & 0xFFFFF000
    return [high | (reg << 7) | 0x37,
            ((value & 0xFFF) << 20) | (reg << 15) | (reg << 7) | 0x13]


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else "build/animus_model_cli.exe").resolve()
    rng = random.Random(20261106)
    pairs = [(0, 0), (MASK, 0), (0x80000000, MASK), (MASK, 2),
             (0x80000000, 31), (0x7FFFFFFF, 0x80000000)]
    pairs += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(24)]
    program, expected = [], []
    for a, b in pairs:
        sa, sb = signed(a), signed(b)
        shift = b & 31
        r_results = [a + b, a << shift, int(sa < sb), int(a < b), a ^ b,
                     a >> shift, a | b, a & b]
        operations = [(f3, 0, result) for f3, result in enumerate(r_results)]
        operations += [(0, 32, a - b), (5, 32, sa >> shift)]
        div = quotient(sa, sb)
        m_results = [a * b, (sa * sb) >> 32, (sa * b) >> 32, (a * b) >> 32,
                     div, MASK if b == 0 else a // b,
                     sa if b == 0 else sa - div * sb, a if b == 0 else a % b]
        operations += [(f3, 1, result) for f3, result in enumerate(m_results)]
        instructions = [((f7 << 25) | (2 << 20) | (1 << 15) | (f3 << 12) | (3 << 7) | 0x33,
                         result) for f3, f7, result in operations]
        immediate = b & 0xFFF
        si = immediate if immediate < 0x800 else immediate - 0x1000
        i_results = {0: a + si, 2: int(sa < si), 3: int(a < (si & MASK)),
                     4: a ^ (si & MASK), 6: a | (si & MASK), 7: a & (si & MASK)}
        instructions += [((immediate << 20) | (1 << 15) | (f3 << 12) | (3 << 7) | 0x13, result)
                         for f3, result in i_results.items()]
        instructions += [((shift << 20) | (1 << 15) | (1 << 12) | (3 << 7) | 0x13, a << shift),
                         ((shift << 20) | (1 << 15) | (5 << 12) | (3 << 7) | 0x13, a >> shift),
                         (((0x400 | shift) << 20) | (1 << 15) | (5 << 12) | (3 << 7) | 0x13, sa >> shift)]
        for instruction, result in instructions:
            program += initialize(1, a) + initialize(2, b) + [instruction]
            expected.append(result & MASK)
    output_dir = Path("build")
    output_dir.mkdir(exist_ok=True)
    binary = output_dir / "integer_oracle.bin"
    binary.write_bytes(b"".join(struct.pack("<I", word) for word in program))
    result = subprocess.run([str(executable), str(binary), str(len(program))],
                            capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    assert len(lines) == len(program), "Wrong number of instruction records"
    for index, reference in enumerate(expected):
        record = json.loads(lines[index * 5 + 4])
        assert len(record["writes"]) == 32
        assert {write["lane"] for write in record["writes"]} == set(range(32))
        assert all(write["reg"] == 3 and write["after"] == reference for write in record["writes"]), (
            index, reference, record)
    print(f"PASS Python integer oracle: {len(expected)} vectors, all 32 lanes")


if __name__ == "__main__":
    main()
