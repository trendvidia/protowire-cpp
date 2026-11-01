#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 TrendVidia, LLC.
"""Generate the pxf.BigFloat text-form corpus (issues #393, #406).

Expected values are computed here with exact rational arithmetic, from
the rule in draft -01 § PXF Annotations, independently of any port:

  * Writing: a BigFloat (mantissa M, exponent E, prec P, negative S) is
    the value (-1)^S * M * 2^E. It is written with N significant decimal
    digits, N = (number of decimal digits of 2^P) + 1, rounded to
    nearest with ties to even, trailing zeros removed, in the form of C's
    %g with precision N: exponent form (d.ddd e+XX, at least two exponent
    digits) when the decimal exponent is below -4 or at least N,
    positional otherwise, followed by the precision suffix "p<P>". The
    value is M rounded to P bits first (#410: a mantissa wider than P is
    the rounded value). A zero is "0p<P>" or "-0p<P>"; only a prec-0
    zero is written without a suffix ("0", or "-0").
  * Reading: a literal with a suffix "p<P>" is read at exactly P bits,
    one without at 256 bits, rounded to nearest with ties to even. It is
    stored as mantissa = the significand as a P-bit integer (top bit
    set), exponent = binary exponent - P, prec = P. A zero is stored
    with an empty mantissa, exponent 0, prec P (0 without a suffix) and
    negative for a sign.

Each enc-<name> case is a PB message (enc-<name>.binpb) and the PXF text
an encoder MUST write for it (enc-<name>.pxf). Each dec-<name> case is a
PXF document (dec-<name>.pxf) and the PB bytes a decoder MUST produce
(dec-<name>.binpb). Each err-<name>.pxf is a document a decoder MUST
reject. Run from anywhere; it writes next to itself, and is
idempotent.
"""
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent

MAX_NUMERIC_LITERAL_DIGITS = 4096


def digits_for(prec: int) -> int:
    """N, the significant digits a prec-bit value is written with."""
    return len(str(2 ** prec)) + 1


# The largest precision a suffix may state: its N is within the limit.
MAX_PREC = 1
while digits_for(MAX_PREC + 1) <= MAX_NUMERIC_LITERAL_DIGITS:
    MAX_PREC += 1


def varint(n: int) -> bytes:
    n &= 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def bigfloat_pb(mant: int, exp: int, prec: int, neg: bool) -> bytes:
    """H { bf = BigFloat{...} } with proto3 zero fields omitted, in
    field-number order (deterministic marshaling)."""
    sub = b""
    if mant:
        mb = mant.to_bytes((mant.bit_length() + 7) // 8, "big")
        sub += b"\x0a" + varint(len(mb)) + mb
    if exp:
        sub += b"\x10" + varint(exp)
    if prec:
        sub += b"\x18" + varint(prec)
    if neg:
        sub += b"\x20\x01"
    return b"\x0a" + varint(len(sub)) + sub


def round_half_even(x: Fraction) -> int:
    q, r = divmod(x.numerator, x.denominator)
    twice = 2 * r
    if twice > x.denominator or (twice == x.denominator and q % 2 == 1):
        q += 1
    return q


def round_to_prec(v: Fraction, p: int) -> Fraction:
    """v rounded to p significant bits, half to even."""
    e2 = 0  # 2^(p-1) <= v / 2^e2 < 2^p
    while v / Fraction(2) ** e2 >= 2 ** p:
        e2 += 1
    while v / Fraction(2) ** e2 < 2 ** (p - 1):
        e2 -= 1
    m = round_half_even(v / Fraction(2) ** e2)
    return Fraction(m) * Fraction(2) ** e2


def write_literal(mant: int, exp: int, prec: int, neg: bool) -> str:
    sign = "-" if neg else ""
    if mant == 0 or prec == 0:
        return f"{sign}0" if prec == 0 else f"{sign}0p{prec}"
    # A mantissa wider than prec bits: the value is the rounded one.
    v = round_to_prec(Fraction(mant) * (Fraction(2) ** exp), prec)
    n = digits_for(prec)
    k = 0  # decimal exponent: 10^k <= v < 10^(k+1)
    while Fraction(10) ** (k + 1) <= v:
        k += 1
    while Fraction(10) ** k > v:
        k -= 1
    q = round_half_even(v / Fraction(10) ** (k - n + 1))
    if q == 10 ** n:
        q //= 10
        k += 1
    digits = str(q).rstrip("0") or "0"
    if k < -4 or k >= n:
        body = digits[0] + ("." + digits[1:] if len(digits) > 1 else "")
        return f"{sign}{body}e{'-' if k < 0 else '+'}{abs(k):02d}p{prec}"
    if k >= 0:
        ip, fp = digits[: k + 1].ljust(k + 1, "0"), digits[k + 1 :]
        return sign + ip + ("." + fp if fp else "") + f"p{prec}"
    return sign + "0." + "0" * (-k - 1) + digits + f"p{prec}"


def read_literal(lit: str):
    neg = lit.startswith("-")
    s = lit.lstrip("+-")
    s, _, suffix = s.partition("p")
    mant_part, _, exp_part = s.lower().partition("e")
    ip, _, fp = mant_part.partition(".")
    v = Fraction(int(ip + fp or "0")) / (Fraction(10) ** len(fp))
    if exp_part:
        v *= Fraction(10) ** int(exp_part)
    p = int(suffix) if suffix else 256
    if v == 0:
        return 0, 0, (p if suffix else 0), neg
    e2 = 0  # 2^(p-1) <= v / 2^e2 < 2^p
    while v / Fraction(2) ** e2 >= 2 ** p:
        e2 += 1
    while v / Fraction(2) ** e2 < 2 ** (p - 1):
        e2 -= 1
    m = round_half_even(v / Fraction(2) ** e2)
    if m == 2 ** p:
        m //= 2
        e2 += 1
    return m, e2, p, neg


ENCODE = {
    # name: (mantissa, exponent, prec, negative)
    "2pow1024-prec64": (1, 1024, 64, False),
    "three-quarters-negative": (3, -2, 64, True),
    "tenth-as-double": (0x1999999999999A, -56, 53, False),
    "one-and-a-half": (3, -1, 53, False),
    "long-mantissa": (2 ** 100 - 1, -50, 100, False),
    "tiny-negative-exponent": (1, -16385, 64, False),
    "prec-zero": (0, 0, 0, False),
    "integer": (1000, 0, 64, False),
    # #410: a zero keeps its precision and sign; a wide mantissa is the
    # value rounded to prec; the exponent bound at the 256-bit default;
    # a literal of MaxNumericLiteralDigits digits plus exponent and suffix.
    "zero-prec64": (0, 0, 64, False),
    "zero-prec64-negative": (0, 0, 64, True),
    "wide-mantissa-prec53": (2 ** 100 - 1, -50, 53, False),
    "exponent-bound-prec256": (1, -16385, 256, False),
    "max-precision-with-exponent": (2 ** MAX_PREC - 1, 16384 - MAX_PREC, MAX_PREC, False),
}

DECODE = {
    # name: literal
    "tenth-unsuffixed": "0.1",
    "tenth-p53": "0.10000000000000001p53",
    "minus-two-and-a-half": "-2.5",
    "1e4932": "1e4932",
    "zero": "0",
    "hundred-digits-unsuffixed": "1" + "2" * 99 + "e-50",
    "hundred-digits-p333": "1" + "2" * 99 + "e-50p333",
    "small-positional": "0.000125",
    "2pow1024-p64": "1.79769313486231590773e+308p64",
    "integer-p64": "1000p64",
    "exponent-only-p24": "1e-5p24",
    # 0.999999 at 8 bits rounds up to 2^8, past the significand: 1 at 8 bits.
    "rounds-up-to-power-p8": "0.999999p8",
    "negative-zero": "-0",
    # The largest precision whose N, 4096, is within MaxNumericLiteralDigits.
    "one-at-max-precision": f"1p{MAX_PREC}",
    # #410: a zero with a precision and sign; the producer's own literals
    # at the exponent bound and at the digit limit read back.
    "zero-p64": "0p64",
    "negative-zero-p64": "-0p64",
    "exponent-bound-prec256": write_literal(1, -16385, 256, False),
    "max-precision-with-exponent": write_literal(2 ** MAX_PREC - 1, 16384 - MAX_PREC, MAX_PREC, False),
}

# Documents a decoder MUST reject: a precision suffix on a field that is
# not a pxf.BigFloat (the error names the field, count), a zero
# precision, and a precision whose N exceeds MaxNumericLiteralDigits,
# one past the limit and one that overflows every fixed-width integer.
REJECT = {
    "suffix-on-int32": "count = 5p64",
    "precision-zero": "bf = 1.5p0",
    "precision-past-limit": f"bf = 1p{MAX_PREC + 1}",
    "precision-overflow": "bf = 1p99999999999999999999999",
}


def main() -> None:
    for name, (m, e, p, s) in ENCODE.items():
        (HERE / f"enc-{name}.binpb").write_bytes(bigfloat_pb(m, e, p, s))
        (HERE / f"enc-{name}.pxf").write_text(f"bf = {write_literal(m, e, p, s)}\n")
    for name, lit in DECODE.items():
        (HERE / f"dec-{name}.pxf").write_text(f"@type bf.v1.H\n\nbf = {lit}\n")
        (HERE / f"dec-{name}.binpb").write_bytes(bigfloat_pb(*read_literal(lit)))
    for name, body in REJECT.items():
        (HERE / f"err-{name}.pxf").write_text(f"@type bf.v1.H\n\n{body}\n")
    print(f"wrote {len(ENCODE)} encode, {len(DECODE)} decode and {len(REJECT)} reject cases")


if __name__ == "__main__":
    main()
