#!/usr/bin/env python3
"""Limit this build to the reviewed WiFi/SCM changes over the gpu72 build branch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = 'cd1066e703e7c023767dfa7be54d5e2109658087'
ALLOWED = {
    'drivers/NativeAdrenoPicasso/PicassoPil.h',
    'drivers/NativeWlan/WlanPil.h', 'drivers/NativeWlan/WlanMpss.h',
    'drivers/NativeWlan/WlanMpss.cpp', 'drivers/NativeWlan/Test/pil.cpp',
    'scripts/build-arm-driver.py',
    '.github/workflows/compose-bootimg.yml',
    'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp',
    'drivers/NativeWlan/WlanBoard.h', 'drivers/NativeWlan/WlanProbe.cpp',
    'docs/picasso-wlan-bringup.md', 'docs/usb-console-bringup.md',
    'scripts/capture-picasso-usb-console.ps1', 'scripts/picasso-windows-serial.cs',
    'scripts/check-picasso-scm-resume.py', 'scripts/check-picasso-capture-command.ps1',
    'scripts/check-picasso-wlan-pas.py', 'scripts/check-picasso-wifi-baseline.py',
    'scripts/build-picasso-fastboot-relay.py', 'scripts/picasso-fastboot-init.S',
    'drivers/NativeUfs/NativeUfs.cpp', 'drivers/NativeWlan/WlanStorage.h',
    'drivers/NativeWlan/WlanFirmware.c', 'drivers/NativeWlan/WlanFirmware.h',
    'drivers/NativeWlan/Test/storage.c', 'scripts/check-picasso-wlan-storage.py',
    'scripts/check-picasso-wlan.py', 'scripts/tests/picasso-wlan-kernel-api.h',
    'scripts/tests/picasso-wlan-storage-adapter.cpp',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    subprocess.run(['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'], cwd=ROOT, check=True)
    changed = set(subprocess.check_output(['git', 'diff', '--name-only', BASE, '--'], cwd=ROOT, text=True).splitlines())
    changed.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, text=True).splitlines())
    if changed - ALLOWED:
        raise SystemExit('Unexpected source changes: ' + ', '.join(sorted(changed - ALLOWED)))
    files = sorted(name for name in ALLOWED if (ROOT / name).is_file())
    report = dict(passed=True, base=BASE, head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  changed=sorted(changed), sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in files})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: changes are limited to WiFi/SCM diagnostics and the Fastboot relay; gpu72 board/seed/ABI inputs retained')


if __name__ == '__main__':
    main()
