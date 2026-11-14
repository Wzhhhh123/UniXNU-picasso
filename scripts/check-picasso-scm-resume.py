#!/usr/bin/env python3
"""Check the production PAS resume call's register arguments and ARM64 SMC shim.

Runs the extracted call against a recording transport, including 64-bit x6
tokens, and cross-compiles the unmodified raw SMC shim. Does not invoke SMC,
authenticate firmware, build XNU or qualify device behavior.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'drivers/NativeAdrenoPicasso/Adreno620Cp.cpp')
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Pass --cxx or put clang++ on PATH')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = args.source.read_text(encoding='utf-8')
    shim = source[source.index('struct ScmReply {'):source.index('\nvoid scmProbe()')]
    signature = re.search(r'ScmReply scmSipCall\([^{}]+\)', shim).group()
    calls = re.findall(r'(?m)^\s*reply = scmSipCall\(1u,[^;]+;', source)
    if len(calls) != 1:
        raise ValueError('Expected exactly one production PAS resume call')
    call = calls[0].strip()
    modem_call = re.search(r'const ScmReply modem = scmSipCall\([^;]+;', source).group()
    fixture = '''#include <cassert>
#include <cstdint>
#include <cstdio>
#include "WlanBoard.h"
struct ScmReply { uint32_t a0, a1, a2, a3; uint64_t a6; };
static uint64_t sent[7];
SIGNATURE {
    sent[0]=function; sent[1]=arginfo; sent[2]=arg0; sent[3]=arg1;
    sent[4]=arg2; sent[5]=arg3; sent[6]=arg6;
    return ScmReply{0, 0, 0, 0, 0};
}
int main() {
    const uint64_t tokens[] = {0, 1, 0x12345678, 0x87654321abcdef01ULL, ~0ULL};
    const uint32_t arginfo=0x82;
    const uint64_t arg0=13, arg1=0x18bc18000ULL, arg2=0x2000;
    for (uint64_t token : tokens) {
        ScmReply reply{1, 0, 0, 0, token};
        CALL
        assert(sent[0] == 1); // Resume the suspended call, do not reissue its FID.
        assert(sent[1] == arginfo && sent[2] == arg0 && sent[3] == arg1 && sent[4] == arg2);
        assert(sent[5] == 0); // No fourth PAS argument / extended-argument pointer.
        assert(sent[6] == token); // ARM_SMCCC_QUIRK_QCOM_A6, all 64 bits.
        assert(reply.a0 == 0);
    }
    const uint32_t kStdSipSmc64 = 0x42000000u;
    MODEM_QUERY
    assert(modem.a0 == 0 && sent[0] == 0x42000207u && sent[1] == 1);
    assert(sent[2] == 4 && sent[3] == 0 && sent[4] == 0 && sent[5] == 0 && sent[6] == 0);
    puts("PASS: five interrupted-call tokens preserve x6 and the PAS arguments");
    puts("PASS: MPSS PAS query uses 4, not ADSP / remote PID 1");
}
'''.replace('SIGNATURE', signature).replace('CALL', call).replace('MODEM_QUERY', modem_call)
    (out / 'ScmResume.cpp').write_text(fixture, encoding='utf-8')
    (out / 'ScmRaw.cpp').write_text('typedef unsigned int uint32_t;\ntypedef unsigned long long uint64_t;\n' + shim, encoding='utf-8')
    report = dict(source=str(args.source.resolve()), source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  scope=__doc__, resume_call=call, hardware_tested=False, kernel_sdk_compiled=False, runs=[])
    env = dict(os.environ)
    env['PATH'] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get('PATH', '')
    exe = out / ('ScmResume.exe' if os.name == 'nt' else 'ScmResume')
    commands = [
        ('host-compile', [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
                          '-I', str(ROOT / 'drivers/NativeWlan'), str(out / 'ScmResume.cpp'), '-o', str(exe)]),
        ('host-run', [str(exe)]),
        ('arm64-compile', [args.cxx, '-target', 'arm64-apple-ios17.0', '-std=c++17', '-ffreestanding',
                           '-fno-exceptions', '-fno-rtti', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-c', str(out / 'ScmRaw.cpp'), '-o', str(out / 'ScmRaw.o')]),
    ]
    for name, command in commands:
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
        (out / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        report['runs'].append(dict(name=name, command=command, exit_code=result.returncode))
        report['passed'] = all(run['exit_code'] == 0 for run in report['runs'])
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(result.stdout + result.stderr, end='')
        if result.returncode:
            return 1
    print('PASS: production PAS resume call and ARM64 SMC shim')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
