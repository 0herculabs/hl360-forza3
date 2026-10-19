#!/usr/bin/env python3
"""Decode imported FM3 XDC previews without running the game or modifying saves.

Requires Pillow. Supports the observed cxds v2 raw-deflate container and tiled
512x256 8_8_8_8 base images. Unsupported layouts fail rather than guessing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

from PIL import Image
from macos_files import fixture_entries


def decode(data):
    if len(data) < 20:
        raise ValueError('Truncated XDC header')
    magic, version, size, compressed, repeated_size = struct.unpack('>5I', data[:20])
    if (magic, version) != (0x63786473, 2) or size != repeated_size:
        raise ValueError('Unsupported XDC container')
    if compressed != len(data) - 20 or size != 655412:
        raise ValueError('Unexpected XDC size')
    stream = zlib.decompressobj(-15)
    raw = stream.decompress(data[20:], size + 1)
    if len(raw) != size or not stream.eof or stream.unused_data:
        raise ValueError('Invalid deflate payload')
    fetch = struct.unpack('>6I', raw[28:52])
    width, height = (fetch[2] & 8191) + 1, ((fetch[2] >> 13) & 8191) + 1
    pitch = ((fetch[0] >> 22) & 511) * 32
    if (width, height, pitch, fetch[0] >> 31, fetch[1] & 255) != (512, 256, 512, 1, 0x86):
        raise ValueError('Unsupported texture layout')
    pixels = raw[52:]
    output = bytearray()
    # Same tiled address formula as SDK pipeline/texture/util.cpp GetTiledOffset2D.
    for y in range(height):
        for x in range(width):
            macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9
            micro = ((x & 7) + ((y & 14) << 2)) << 2
            offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 1) << 4)
            offset = (((offset & ~511) << 3) + ((y & 16) << 7)
                      + ((offset & 448) << 2)
                      + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63))
            output.extend(pixels[offset:offset + 4][::-1])  # 8-in-32 endian swap.
    return Image.frombytes('RGBA', (width, height), bytes(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('user', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for path, directory in fixture_entries(args.user):
        if directory or not (path.name.startswith('Thumbnail_') and path.suffix == '.xdc'):
            continue
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        target = args.output / (path.stem + '-' + digest[:12] + '.png')
        if target.resolve() == path.resolve():
            raise ValueError('Output must not overwrite input')
        decode(data).save(target)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
        report.append({'source': str(path.resolve()), 'sha256': digest,
                       'image': str(target.resolve())})
    (args.output / 'inspection.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Decoded {len(report)} previews; source files unchanged: {args.output}')


if __name__ == '__main__':
    main()
