"""Independent finite-FP32 oracle using exact rational arithmetic (standard library only).

Arithmetic/FMA/sqrt results and flags are checked for all five rounding modes.
SFU is checked separately against Decimal references; its tolerance is a test
criterion for HostMathV1, not an approved hardware SFU error specification.
"""
from decimal import Decimal, localcontext
from fractions import Fraction
import json
from math import isqrt
from pathlib import Path
import random
import struct
import subprocess
import sys

MASK = (1 << 32) - 1


def power2(exponent):
    return Fraction(1 << exponent) if exponent >= 0 else Fraction(1, 1 << -exponent)


def fraction(bits):
    exponent = (bits >> 23) & 255
    assert exponent != 255
    mantissa = bits & 0x7FFFFF
    value = Fraction(mantissa) * power2(-149) if exponent == 0 else Fraction((1 << 23) | mantissa) * power2(exponent - 150)
    return -value if bits >> 31 else value


def log2_floor(value):
    estimate = value.numerator.bit_length() - value.denominator.bit_length()
    return estimate - 1 if value < power2(estimate) else estimate


def overflow(sign, mode):
    infinity = mode in (0, 4) or mode == (2 if sign else 3)
    return (sign << 31) | (0x7F800000 if infinity else 0x7F7FFFFF), 5


def pack(sign, exponent, mantissa, inexact, mode):
    if exponent >= -126:
        if mantissa >= (1 << 24):
            mantissa >>= 1
            exponent += 1
        if exponent > 127:
            return overflow(sign, mode)
        return (sign << 31) | ((exponent + 127) << 23) | (mantissa & 0x7FFFFF), int(inexact)
    # Caller computes tininess independently at unbounded-exponent precision.
    return (sign << 31) | mantissa, int(inexact) | (2 if inexact and mantissa < (1 << 23) else 0)


def rounded(value, mode):
    if value == 0:
        return (0x80000000 if mode == 2 else 0), 0  # Exact cancellation in these nonzero test inputs.
    sign = int(value < 0)
    value = abs(value)
    exponent = log2_floor(value)
    if exponent > 127:
        return overflow(sign, mode)
    scaled = value / power2(max(exponent - 23, -149))
    q, remainder = divmod(scaled.numerator, scaled.denominator)
    twice = remainder * 2
    increment = ((mode == 0 and (twice > scaled.denominator or twice == scaled.denominator and q % 2))
                 or (mode == 4 and twice >= scaled.denominator)
                 or (mode == 2 and sign and remainder) or (mode == 3 and not sign and remainder))
    bits, flags = pack(sign, exponent, q + bool(increment), remainder != 0, mode)
    if exponent < -126 and remainder:
        # IEEE after-rounding tininess uses a separate 24-bit rounding with an
        # unbounded exponent, NOT the delivered (possibly min-normal) value.
        # See https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat-FAQ.html
        unbounded = value / power2(exponent - 23)
        uq, ur = divmod(unbounded.numerator, unbounded.denominator)
        ud = unbounded.denominator
        up = ((mode == 0 and (2*ur > ud or 2*ur == ud and uq % 2))
              or (mode == 4 and 2*ur >= ud) or (mode == 2 and sign and ur)
              or (mode == 3 and not sign and ur))
        tiny = Fraction(uq + bool(up)) * power2(exponent - 23) < power2(-126)
        flags = 1 | (2 if tiny else 0)
    return bits, flags


def rounded_sqrt(value, mode):
    assert value > 0
    exponent = log2_floor(value) // 2
    scaled_square = value / power2(2 * max(exponent - 23, -149))
    n, d = scaled_square.numerator, scaled_square.denominator
    q = isqrt(n // d)
    inexact = q * q * d != n
    # Compare sqrt(n/d) with q+1/2 by squaring, with no approximate host sqrt.
    midpoint_difference = 4 * n - (2 * q + 1) ** 2 * d
    increment = ((mode == 0 and (midpoint_difference > 0 or midpoint_difference == 0 and q % 2))
                 or (mode == 4 and midpoint_difference >= 0) or (mode == 3 and inexact))
    return pack(0, exponent, q + bool(increment), inexact, mode)


def initialize_x(register, value):
    high = (value + 0x800) & 0xFFFFF000
    return [high | (register << 7) | 0x37,
            ((value & 0xFFF) << 20) | (register << 15) | (register << 7) | 0x13]


def initialize_f(register, bits):
    return initialize_x(register, bits) + [0xF0000053 | (register << 15) | (register << 7)]


def execute(executable, path, words):
    path.write_bytes(b"".join(struct.pack("<I", word) for word in words))
    process = subprocess.run([str(executable), str(path), str(len(words)), "--experimental-numeric"],
                             capture_output=True, text=True, check=True)
    records = process.stdout.splitlines()
    assert len(records) == len(words)
    return records


def arithmetic(executable, directory):
    rng = random.Random(20261107)
    triples = [(0x3F800000, 0x33800000, 0xBF800000), (0x3F800001, 0x3F7FFFFE, 0xBF800000),
               (1, 0x3F000000, 0x00800000), (0x7F7FFFFF, 0x40000000, 0xFF7FFFFF),
               (0x80000001, 0x3F000000, 0x80800000), (0x00800000, 0x3F800001, 0x80800000),
               (0xBF800000, 0xB3800000, 0x3F800000), (0x3F800000, 0x3F800000, 0x3F800000)]
    while len(triples) < 20:
        values = tuple(rng.getrandbits(32) for _ in range(3))
        if all(0 < (value & 0x7FFFFFFF) < 0x7F800000 for value in values):
            triples.append(values)
    program, expected = [], []
    for aa, bb, cc in triples:
        for mode in range(5):
            for operation in range(9):
                a = aa & 0x7FFFFFFF if operation == 8 else aa
                fa, fb, fc = fraction(a), fraction(bb), fraction(cc)
                if operation < 4:
                    f7 = (0, 4, 8, 12)[operation]
                    instruction = (f7 << 25) | (2 << 20) | (1 << 15) | (mode << 12) | (3 << 7) | 0x53
                    exact = (fa + fb, fa - fb, fa * fb, fa / fb)[operation]
                    reference = rounded(exact, mode)
                elif operation < 8:
                    opcode = (0x43, 0x47, 0x4B, 0x4F)[operation - 4]
                    instruction = (4 << 27) | (2 << 20) | (1 << 15) | (mode << 12) | (3 << 7) | opcode
                    exact = (fa * fb + fc, fa * fb - fc, -fa * fb + fc, -fa * fb - fc)[operation - 4]
                    reference = rounded(exact, mode)
                else:
                    instruction = (44 << 25) | (1 << 15) | (mode << 12) | (3 << 7) | 0x53
                    reference = rounded_sqrt(fa, mode)
                # Clear accumulated fflags for each independent vector. frm is not used (static rm).
                program += [0x00105073] + initialize_f(1, a) + initialize_f(2, bb) + initialize_f(4, cc) + [instruction]
                expected.append(reference)
    records = execute(executable, directory / "float_oracle.bin", program)
    for index, (bits, flags) in enumerate(expected):
        record = json.loads(records[index * 11 + 10])
        assert len(record["writes"]) == 32
        assert {write["lane"] for write in record["writes"]} == set(range(32))
        assert all(write["file"] == "f" and write["reg"] == 3 and write["after"] == bits for write in record["writes"]), (index, hex(bits), record)
        csr = record["csr_writes"]
        assert len(csr) == (32 if flags else 0), (index, flags, csr)
        assert all(write["after_fcsr"] == flags for write in csr), (index, flags, csr)
    print(f"PASS exact FP32 oracle: {len(expected)} vectors, five rounding modes, results and flags, all 32 lanes")


def decimal_sin_cos(value, cosine):
    term = Decimal(1) if cosine else value
    total = term
    for index in range(1, 150):
        denominator = (2 * index - 1) * (2 * index) if cosine else (2 * index) * (2 * index + 1)
        term = -term * value * value / denominator
        total += term
        if abs(term) < Decimal("1e-110"):
            break
    return total


def ordered(bits):
    return (~bits & MASK) if bits >> 31 else bits | 0x80000000


def sfu_reference(executable, directory):
    program, expected = [], []
    with localcontext() as context:
        context.prec = 120
        for operation in range(6):
            values = [0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0] if operation in (0, 1, 3) else [-4.0, -2.0, -0.5, 0.0, 0.5, 2.0, 4.0]
            for value in values:
                bits = struct.unpack("<I", struct.pack("<f", value))[0]
                rational = fraction(bits)
                x = Decimal(rational.numerator) / Decimal(rational.denominator)
                if operation == 0: reference = 1 / x
                elif operation == 1: reference = 1 / x.sqrt()
                elif operation == 2: reference = Decimal(2) ** x
                elif operation == 3: reference = x.ln() / Decimal(2).ln()
                else: reference = decimal_sin_cos(x, operation == 5)
                expected_bits = 0 if reference == 0 else rounded(Fraction(reference), 0)[0]
                instruction = ((32 + operation) << 25) | (1 << 15) | (3 << 7) | 0x0B
                program += initialize_f(1, bits) + [instruction]
                expected.append(expected_bits)
    records = execute(executable, directory / "sfu_oracle.bin", program)
    for index, expected_bits in enumerate(expected):
        record = json.loads(records[index * 4 + 3])
        assert len(record["writes"]) == 32 and not record["csr_writes"]
        assert record["sfu_profile"] == "host-math-v1"
        assert all(abs(ordered(write["after"]) - ordered(expected_bits)) <= 4 for write in record["writes"]), (index, hex(expected_bits), record)
    print(f"PASS HostMathV1 SFU reference: {len(expected)} finite vectors within 4 ULP (test criterion only)")


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else "build/animus_model_cli.exe").resolve()
    directory = Path("build")
    directory.mkdir(exist_ok=True)
    arithmetic(executable, directory)
    sfu_reference(executable, directory)


if __name__ == "__main__":
    main()
