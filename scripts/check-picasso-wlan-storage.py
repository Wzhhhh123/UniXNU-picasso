#!/usr/bin/env python3
"""Exercise production GPT/FAT16/MDT readers; optional local modem image stays local."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--gpt', type=Path)
    parser.add_argument('--lun-blocks', type=int)
    parser.add_argument('--cc', default=shutil.which('clang'))
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    if args.gpt and (not args.firmware or not args.lun_blocks):
        parser.error('--gpt requires --firmware and the measured --lun-blocks')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    driver = ROOT / 'drivers/NativeWlan'
    runs = []
    ufs = (ROOT / 'drivers/NativeUfs/NativeUfs.cpp').read_text()
    start = ufs.index('// Firmware is read from the named modem partition')
    end = ufs.index('// ---- Persistent Data store window', start)
    adapter = ufs[start:end].replace('#include "../NativeWlan/WlanStorage.h"', '#include "WlanStorage.h"')
    (out / 'modem-adapter.inc').write_text(adapter)
    for sanitized in (False, True):
        name = 'storage' + ('-sanitized' if sanitized else '')
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
        if os.name == 'nt':
            flags += ['-D_CRT_SECURE_NO_WARNINGS']
        if sanitized:
            flags += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        commands = [[args.cc, *flags, '-I', str(driver), str(driver / 'Test/storage.c'),
                     str(driver / 'WlanFirmware.c'), '-o', str(exe)],
                    [str(exe), *([str(args.firmware.resolve())] if args.firmware else []),
                     *([str(args.gpt.resolve()), str(args.lun_blocks)] if args.gpt else [])]]
        adapter_exe = out / ('adapter-' + exe.name)
        commands += [[args.cxx, *['-std=c++17' if flag == '-std=c11' else flag for flag in flags],
                      '-I', str(driver), '-I', str(out),
                      str(ROOT / 'scripts/tests/picasso-wlan-storage-adapter.cpp'), '-o', str(adapter_exe)],
                     [str(adapter_exe)]]
        for index, command in enumerate(commands):
            result = subprocess.run(command, capture_output=True, text=True)
            (out / f'{name}-{index}.log').write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            runs.append(dict(command=command, passed=True, output=result.stdout))
    inputs = [driver / p for p in ('WlanStorage.h', 'WlanFirmware.c', 'WlanFirmware.h', 'Test/storage.c')]
    inputs += [ROOT / 'drivers/NativeUfs/NativeUfs.cpp', ROOT / 'scripts/tests/picasso-wlan-storage-adapter.cpp', Path(__file__)]
    report = dict(passed=True, runs=runs, hardware_tested=False,
                  inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs})
    if args.firmware:
        report['local_firmware_sha256'] = hashlib.sha256(args.firmware.read_bytes()).hexdigest()
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: GPT/FAT16/MDT readers, bounds/corruption regressions and sanitizer')


if __name__ == '__main__':
    main()
