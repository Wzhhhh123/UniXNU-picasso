#!/usr/bin/env python3
"""Build a minimal Fastboot fallback from the qualified local Picasso recovery.

Requires a local recovery image and LLVM; downloads and device writes are never
performed. Temporarily boot and verify the result before any authorized flash.
The original boot partition must be backed up separately. The recovery kernel
and DTB/DTBO are inputs, not distributed by this script.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

BASE_SHA256 = '016833bb24522c2210abdcfa06adb75eeb88629fab7b86c9b389c4e62f9a4d55'
PARTITION_BYTES = 0x8000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(value, page):
    return (value + page - 1) // page * page


def unpack(image):
    if image[:8] != b'ANDROID!' or len(image) < 1660:
        raise ValueError('Not an Android boot image')
    k, _, r, _, s, _, _, page, version = struct.unpack_from('<9I', image, 8)
    if version != 2 or page != 4096 or struct.unpack_from('<I', image, 1644)[0] != 1660:
        raise ValueError('Expected the qualified Android v2, 4096-byte page layout')
    dtbo_size, dtbo_offset = struct.unpack_from('<IQ', image, 1632)
    dtb_size = struct.unpack_from('<I', image, 1648)[0]
    cursor, sections = page, []
    for size in (k, r, s, dtbo_size, dtb_size):
        if len(sections) == 3 and dtbo_offset != cursor:
            raise ValueError('Unexpected recovery DTBO location')
        if cursor + size > len(image):
            raise ValueError('Truncated section')
        sections.append(image[cursor:cursor + size])
        cursor += align(size, page)
    digest = hashlib.sha1()
    for data in sections:
        digest.update(data)
        digest.update(struct.pack('<I', len(data)))
    if digest.digest() + bytes(12) != image[576:608]:
        raise ValueError('Android image ID mismatch')
    if sections[0][56:60] != b'ARMd' or sections[4][:4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('Missing ARM64 Image or FDT')
    return page, sections


def cpio(init):
    result = bytearray()
    entries = [('.', 0o40755, b'', 0, 0), ('dev', 0o40755, b'', 0, 0),
               ('dev/console', 0o20600, b'', 5, 1), ('init', 0o100755, init, 0, 0),
               ('TRAILER!!!', 0, b'', 0, 0)]
    for ino, (name, mode, data, major, minor) in enumerate(entries, 1):
        name = name.encode() + b'\0'
        fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0)
        result += b'070701' + ''.join(f'{n:08x}' for n in fields).encode() + name
        result += bytes(-len(result) % 4)
        result += data
        result += bytes(-len(result) % 4)
    return bytes(result)


def check_init(elf):
    if elf[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', elf, 16) != (2, 183):
        raise ValueError('Init is not an AArch64 static executable')
    entry, phoff = struct.unpack_from('<QQ', elf, 24)
    phsize, phnum = struct.unpack_from('<HH', elf, 54)
    executable_entry = False
    for index in range(phnum):
        kind, flags, offset, va, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize)
        if kind in (2, 3):
            raise ValueError('Init must not require a dynamic linker')
        if kind == 1:
            if offset + filesz > len(elf) or filesz > memsz or flags & 3 == 3:
                raise ValueError('Invalid or writable executable ELF segment')
            executable_entry |= bool(flags & 1 and va <= entry < va + filesz)
    if not executable_entry or b'bootloader\0' not in elf:
        raise ValueError('Missing entry point or reboot target')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    args = parser.parse_args()
    original = args.base.read_bytes()
    if sha(original) != BASE_SHA256:
        raise ValueError('Recovery source differs from the qualified Picasso image')
    page, sections = unpack(original)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name('picasso-fastboot-init.S')
    compiler = args.clang.resolve()
    env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ.get('PATH', ''))
    command = [str(compiler), '--target=aarch64-linux-gnu', '-nostdlib', '-static',
               '-fuse-ld=lld', '-Wl,-e,_start,--build-id=none,-z,max-page-size=4096,--strip-all',
               str(source), '-o', str(out / 'init')]
    subprocess.run(command, env=env, check=True)
    init = (out / 'init').read_bytes()
    check_init(init)
    archive = cpio(init)
    ramdisk = bytearray(gzip.compress(archive, compresslevel=9, mtime=0))
    ramdisk[9] = 255  # Deterministic gzip OS byte across Python/platform versions.
    sections[1] = bytes(ramdisk)
    header = bytearray(original[:page])
    struct.pack_into('<I', header, 16, len(ramdisk))
    dtbo_offset = page + sum(align(len(x), page) for x in sections[:3])
    struct.pack_into('<Q', header, 1636, dtbo_offset)
    old_args = (original[64:576] + original[608:1632]).split(b'\0', 1)[0].decode('ascii')
    tokens = [t for t in old_args.split() if not t.startswith(('rdinit=', 'init=', 'panic=', 'androidboot.init_fatal_reboot_target='))]
    cmdline = ' '.join(tokens + ['rdinit=/init', 'panic=0']).encode()
    if len(cmdline) >= 1536:
        raise ValueError('Command line exceeds Android header')
    cmdline = cmdline.ljust(1536, b'\0')
    header[64:576], header[608:1632] = cmdline[:512], cmdline[512:]
    digest = hashlib.sha1()
    for data in sections:
        digest.update(data)
        digest.update(struct.pack('<I', len(data)))
    header[576:608] = digest.digest() + bytes(12)
    image = bytes(header) + b''.join(data + bytes(-len(data) % page) for data in sections)
    if len(image) > PARTITION_BYTES:
        raise ValueError('Relay exceeds Picasso boot partition')
    _, checked = unpack(image)
    assert checked == sections and gzip.decompress(checked[1]) == archive
    (out / 'picasso-fastboot-relay.img').write_bytes(image)
    report = dict(passed=True, purpose='Fallback boot: Linux RESTART2 bootloader; no iOS or WLAN implementation',
                  base_sha256=BASE_SHA256, image_sha256=sha(image), image_bytes=len(image),
                  init_sha256=sha(init), init_source_sha256=sha(source.read_bytes()),
                  kernel_sha256=sha(sections[0]), dtbo_sha256=sha(sections[3]), dtb_sha256=sha(sections[4]),
                  kernel_args=cmdline.rstrip(b'\0').decode(), compiler_command=command,
                  hardware_tested=False, writes_partitions=False)
    (out / 'relay.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
