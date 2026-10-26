#!/usr/bin/env python3
"""
Converts a PSF1 or PSF2 console font, gzipped or not, to a raw fbcons font:
256 glyphs in CP437 order, one row after another, most significant bit on
the left. The glyphs are found by the Unicode table of the PSF, so the order
of the glyphs in the source does not matter.

A glyph that the source lacks comes from the fallback font. One that neither
has stays blank, and its code is printed.
"""

import argparse
import gzip
import struct
import sys

PSF1_MAGIC = b"\x36\x04"
PSF2_MAGIC = b"\x72\xb5\x4a\x86"

PSF1_MODE512 = 0x01
PSF1_MODEHASTAB = 0x02
PSF1_MODEHASSEQ = 0x04
PSF2_HAS_UNICODE_TABLE = 0x01

PSF1_SEPARATOR = 0xFFFF
PSF1_STARTSEQ = 0xFFFE
PSF2_SEPARATOR = 0xFF
PSF2_STARTSEQ = 0xFE

# CP437 draws a glyph for these codes, but the codec maps them to controls
CP437_LOW = "\0☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
CP437_DELETE = "⌂"


def cp437_char(code):
    if code < 0x20:
        return CP437_LOW[code]
    if code == 0x7F:
        return CP437_DELETE
    return bytes([code]).decode("cp437")


def psf1_unicode_map(table):
    unicode_map = {}
    index = 0
    in_sequence = False

    for offset in range(0, len(table), 2):
        value = table[offset] | (table[offset + 1] << 8)

        if value == PSF1_SEPARATOR:
            index += 1
            in_sequence = False
        elif value == PSF1_STARTSEQ:
            in_sequence = True
        elif not in_sequence:
            unicode_map.setdefault(chr(value), index)

    return unicode_map


def psf2_unicode_map(table, count):
    unicode_map = {}

    for index, entry in enumerate(table.split(bytes([PSF2_SEPARATOR]))[:count]):
        # What follows PSF2_STARTSEQ is a sequence of combining characters
        for char in entry.split(bytes([PSF2_STARTSEQ]))[0].decode("utf-8"):
            unicode_map.setdefault(char, index)

    return unicode_map


def read_psf(path):
    with open(path, "rb") as file:
        data = file.read()

    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)

    if data[:2] == PSF1_MAGIC:
        mode = data[2]
        header_size = 4
        count = 512 if mode & PSF1_MODE512 else 256
        glyph_size = data[3]
        has_table = bool(mode & (PSF1_MODEHASTAB | PSF1_MODEHASSEQ))
    elif data[:4] == PSF2_MAGIC:
        (_, _, header_size, flags, count, glyph_size, _, _) = struct.unpack("<8I", data[:32])
        has_table = bool(flags & PSF2_HAS_UNICODE_TABLE)
    else:
        sys.exit(f"{path}: not a PSF font")

    if not has_table:
        sys.exit(f"{path}: no Unicode table")

    glyphs = [data[header_size + i * glyph_size : header_size + (i + 1) * glyph_size] for i in range(count)]
    table = data[header_size + count * glyph_size :]

    if data[:2] == PSF1_MAGIC:
        return glyphs, psf1_unicode_map(table)

    return glyphs, psf2_unicode_map(table, count)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="PSF font to convert")
    parser.add_argument("output", help="Raw font to write")
    parser.add_argument("--fallback", help="PSF font of the same size, for the glyphs that the source lacks")
    args = parser.parse_args()

    glyphs, unicode_map = read_psf(args.source)
    blank = bytes(len(glyphs[0]))

    fallback_glyphs, fallback_map = [], {}
    if args.fallback:
        fallback_glyphs, fallback_map = read_psf(args.fallback)
        if len(fallback_glyphs[0]) != len(blank):
            sys.exit("the fallback font has another glyph size")

    output = bytearray()
    missing = []

    for code in range(256):
        char = cp437_char(code)

        if char in unicode_map:
            output += glyphs[unicode_map[char]]
        elif char in fallback_map:
            output += fallback_glyphs[fallback_map[char]]
        else:
            output += blank
            if code != 0:
                missing.append(code)

    with open(args.output, "wb") as file:
        file.write(output)

    if missing:
        print("blank glyphs:", " ".join(f"{code:#04x}" for code in missing), file=sys.stderr)


if __name__ == "__main__":
    main()
