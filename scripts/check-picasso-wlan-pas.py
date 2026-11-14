#!/usr/bin/env python3
"""Verify the modem PAS ID from both checked-in Picasso FDTs; no device access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]


def nodes(data):
    if len(data) < 40:
        raise ValueError('Truncated FDT header')
    magic, total, tree, strings, _, version, _, _, strings_size, tree_size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or version < 17 or total > len(data):
        raise ValueError('Invalid FDT header')
    if tree + tree_size > total or strings + strings_size > total:
        raise ValueError('FDT blocks exceed file')
    names = data[strings:strings + strings_size]
    cursor, limit, stack, result = tree, tree + tree_size, [], {}
    while cursor + 4 <= limit:
        token, = struct.unpack_from('>I', data, cursor)
        cursor += 4
        if token == 1:
            end = data.index(0, cursor, limit)
            stack.append(data[cursor:end].decode('ascii'))
            cursor = (end + 4) & ~3
            path = '/'.join(stack)
            if path in result:
                raise ValueError('Duplicate FDT node')
            result[path] = {}
        elif token == 2:
            if not stack:
                raise ValueError('Unbalanced FDT node')
            stack.pop()
        elif token == 3:
            if not stack or cursor + 8 > limit:
                raise ValueError('Truncated FDT property')
            size, name = struct.unpack_from('>II', data, cursor)
            cursor += 8
            if cursor + size > limit or name >= len(names):
                raise ValueError('FDT property outside block')
            key = names[name:names.index(0, name)].decode('ascii')
            props = result['/'.join(stack)]
            if key in props:
                raise ValueError('Duplicate FDT property')
            props[key] = data[cursor:cursor + size]
            cursor = (cursor + size + 3) & ~3
        elif token == 9:
            if stack:
                raise ValueError('Unclosed FDT node')
            return result
        elif token != 4:
            raise ValueError('Invalid FDT token')
    raise ValueError('Missing FDT end')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    header = (ROOT / 'drivers/NativeWlan/WlanBoard.h').read_text()
    declared = int(re.search(r'^#define WLAN_MPSS_PAS_ID (\d+)u$', header, re.M)[1])
    reports = []
    for name in ('picasso.dtb', 'picasso-topology.dtb'):
        path = ROOT / 'uefi/mu-silicium/Resources/DTBs' / name
        data = path.read_bytes()
        tree = nodes(data)
        modem = tree['/soc/qcom,mss@4080000']
        adsp = tree['/soc/qcom,lpass@3000000']
        assert modem['qcom,firmware-name'] == b'modem\0'
        assert modem['qcom,pas-id'] == struct.pack('>I', declared)
        assert declared == 4 and adsp['qcom,pas-id'] == struct.pack('>I', 1)
        region = tree['/reserved-memory/modem_wlan_region@8c000000']
        assert modem['memory-region'] == region['phandle']
        base = int(re.search(r'^#define WLAN_MPSS_BASE (0x[0-9a-f]+)ULL$', header, re.M)[1], 16)
        size = int(re.search(r'^#define WLAN_MPSS_SIZE (0x[0-9a-f]+)ULL$', header, re.M)[1], 16)
        assert region['reg'] == struct.pack('>QQ', base, size)
        reports.append(dict(file=name, sha256=hashlib.sha256(data).hexdigest(), modem_pas=declared,
                            adsp_pas=1, modem_base=base, modem_size=size))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(passed=True, hardware_tested=False, dtbs=reports), indent=2) + '\n')
    print('PASS: both Picasso DTBs identify MPSS as PAS 4; PAS 1 is ADSP')


if __name__ == '__main__':
    main()
