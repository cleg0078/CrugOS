#!/usr/bin/env python3
"""Boot the kernel with `trap.test=<recipe>` and check its crash reports.

Phase H3 replaced the old one-line panic for every non-breakpoint exception
with a structured report (kernel/lib/trapfmt.c, kernel/arch/x86_64/idt.c) and
a three-way policy: handled, user-killable, fatal.  This gate injects one
fault per case and requires the exact report lines, the IST stack number,
the panic naming the exception, and exit status 35 - plus one control boot
that still reaches the phase marker with exit status 33.

Every crafted fault runs just before the kernel would power off, so the
report is the last thing on the serial line before the panic.

usage: run-trap.py --disk build/x86_64/disk.img \
                   --serial-log build/x86_64/serial.log \
                   --exit-code build/x86_64/exit-code.txt
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

PANIC_HEADER = 'CRUGOS PANIC: '
FORBIDDEN = ('trap test unknown', 'trap test survived')
CONTROL_PHASE_RE = re.compile(r'CRUGOS: phase [0-9a-z]+ boot ok')
CONTROL_MARKERS = ('CRUGOS: selftest ok',)

CASES = [
    {'name': 'control', 'exit': 33},
    {'name': 'de', 'vector': 0, 'name_text': 'divide error', 'ist': 0,
     'exit': 35},
    {'name': 'ud', 'vector': 6, 'name_text': 'invalid opcode', 'ist': 0,
     'exit': 35},
    {'name': 'gp', 'vector': 13, 'name_text': 'general protection',
     'ist': 0, 'exit': 35},
    {'name': 'pf', 'vector': 14, 'name_text': 'page fault', 'ist': 0,
     'exit': 35, 'extra': ('write=yes', 'cr2=0xffff800000000000')},
    {'name': 'df', 'vector': 8, 'name_text': 'double fault', 'ist': 1,
     'exit': 35},
    {'name': 'nmi', 'vector': 2, 'name_text': 'NMI', 'ist': 2, 'exit': 35},
    {'name': 'mc', 'vector': 18, 'name_text': 'machine check', 'ist': 3,
     'exit': 35},
    {'name': 'db', 'vector': 1, 'name_text': 'debug', 'ist': 4, 'exit': 35},
]


def fail(message):
    print("FAIL: " + message)
    return 1


def log_text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def tail(text, count=12):
    rows = [r for r in text.splitlines() if r.strip()]
    return chr(10).join(rows[-count:])


def boot(args, image, log, exit_code, recipe):
    extra = '' if recipe is None else 'trap.test=' + recipe
    cmd = [
        args.make, '--no-print-directory', 'run',
        'DISK_IMG=' + str(image),
        'CRYPT_PASS=',
        'CMDLINE_EXTRA=' + extra,
        'QEMU_DISPLAY=none',
        'NET_MODE=none',
        'SERIAL_TCP=',
        'SERIAL_LOG=' + str(log),
        'QEMU_EXIT=' + str(exit_code),
        'TIMEOUT_SEC=%d' % args.timeout,
    ]
    try:
        subprocess.run(cmd, cwd=args.repo, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT,
                       timeout=args.timeout + 120)
    except subprocess.TimeoutExpired:
        return 'the make run wrapper did not finish'
    if not exit_code.exists():
        return 'QEMU wrote no exit status (timeout or triple fault)'
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo',
                    default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument('--make', default='make')
    ap.add_argument('--disk', required=True)
    ap.add_argument('--serial-log', required=True)
    ap.add_argument('--exit-code', required=True)
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    disk = Path(args.disk)
    if not disk.exists():
        return fail("missing disk image %s" % disk)

    base = Path(args.serial_log)
    case_dir = base.parent / 'trap'
    if case_dir.exists():
        shutil.rmtree(case_dir)
    case_dir.mkdir(parents=True, exist_ok=True)
    print("trap: case dir %s" % case_dir)

    reported = 0
    control_ok = False
    for case in CASES:
        name = case['name']
        recipe = None if name == 'control' else name
        image = case_dir / (name + '.img')
        log = case_dir / (name + '.log')
        exit_code = case_dir / (name + '.exit')
        shutil.copyfile(disk, image)
        error = boot(args, image, log, exit_code, recipe)
        text = log_text(log)
        if error is not None:
            return fail('case %s: %s' % (name, error) + chr(10) + tail(text))
        raw = exit_code.read_text().strip()
        status = int(raw) if raw else -1
        if status != case['exit']:
            return fail('case %s exited %d, expected %d'
                        % (name, status, case['exit']) + chr(10) + tail(text))
        for marker in FORBIDDEN:
            if marker in text:
                return fail('case %s printed %r' % (name, marker)
                            + chr(10) + tail(text))
        if name == 'control':
            if not CONTROL_PHASE_RE.search(text):
                return fail('control boot never printed the phase marker'
                            + chr(10) + tail(text))
            for marker in CONTROL_MARKERS:
                if marker not in text:
                    return fail('control boot never printed %r' % marker
                                + chr(10) + tail(text))
            if 'CRUGOS: panic' in text:
                return fail('the control boot panicked' + chr(10) + tail(text))
            control_ok = True
            print('ok: trap control boot (exit 33)')
        else:
            if 'CRUGOS: trap test ' + name not in text:
                return fail('case %s never announced its recipe' % name
                            + chr(10) + tail(text))
            head = ('CRUGOS: trap report ' + case['name_text'] +
                    ' vector=%d error=' % case['vector'])
            if head not in text:
                return fail('case %s is missing %r' % (name, head)
                            + chr(10) + tail(text))
            need = [' ist=%d from=kernel' % case['ist']]
            need.extend(case.get('extra', ()))
            for frag in need:
                if frag not in text:
                    return fail('case %s is missing %r' % (name, frag)
                                + chr(10) + tail(text))
            panic = PANIC_HEADER + case['name_text']
            if panic not in text:
                return fail('case %s panicked without naming the exception'
                            % name + chr(10) + tail(text))
            if 'CRUGOS: panic' not in text:
                return fail('case %s never reached the panic path' % name
                            + chr(10) + tail(text))
            reported += 1
            print('ok: trap %s -> %s (ist=%d, exit 35)'
                  % (name, case['name_text'], case['ist']))
        if not args.keep:
            image.unlink()

    if not control_ok:
        return fail('the control boot never ran')
    hostile = len(CASES) - 1
    if reported != hostile:
        return fail('only %d of %d crafted faults were reported'
                    % (reported, hostile))
    print('ok: trap policy reported %d crafted fault(s) (control boot exit 33)'
          % reported)
    return 0


if __name__ == '__main__':
    sys.exit(main())
