#!/usr/bin/env python3
"""Build the standalone native checker and compare fresh processes, offline."""
import argparse
import difflib
import os
from pathlib import Path
import re
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', default='build-native', help='Configured static-library build directory')
parser.add_argument('--external-only', action='store_true', help='Check a COOLPROP_EXTERNAL_FLUIDS build')
parser.add_argument('--action', help='Run only one external-mode check (for diagnostics)')
args = parser.parse_args()
build = (root / args.build).resolve()
results = root / 'dev/external_fluids_check/results' / build.name
results.mkdir(parents=True, exist_ok=True)
flags = {}
for line in (build / 'CMakeFiles/CoolProp.dir/flags.make').read_text().splitlines():
    if ' = ' in line:
        key, value = line.split(' = ', 1)
        flags[key] = shlex.split(value)
compiler = os.environ.get('CXX', 'c++')
binary = results / 'check'
subprocess.run([compiler, *flags['CXX_DEFINES'], *flags['CXX_INCLUDES'], *flags['CXX_FLAGS'],
                str(root / 'dev/external_fluids_check/check.cpp'), str(build / 'libCoolProp.a'),
                '-pthread', '-ldl', '-o', str(binary)], check=True, cwd=root)
data = str(root / 'dev/fluids_cbor')


def run(mode, action, *extra):
    return subprocess.check_output([str(binary), mode, action, data, *extra], cwd=root, timeout=120)


def compare(action, *extra):
    embedded = run('embedded', action, *extra)
    external = run('external', action, *extra)
    label = action + ('-' + extra[0].encode().hex() if extra else '')
    (results / (label + '-embedded.txt')).write_bytes(embedded)
    (results / (label + '-external.txt')).write_bytes(external)
    if embedded != external:
        diff = ''.join(difflib.unified_diff(embedded.decode().splitlines(True), external.decode().splitlines(True),
                                         fromfile='embedded', tofile='external'))
        (results / (label + '.diff')).write_text(diff)
        raise AssertionError('Outputs differ for ' + label + '\n' + diff[:4000])
    return embedded


if args.action:
    print(run('external', args.action).decode(), end='')
    raise SystemExit(0)

subprocess.run(['python3', str(root / 'dev/external_fluids_check/check_generator.py')], check=True, cwd=root)
if args.external_only:
    print(run('embedded', 'missing').decode(), end='')
    fluids = run('external', 'list').decode().strip().split(',')
    reference = root / 'dev/external_fluids_check/results/build-native'
    outputs = 0
    errors = 0
    for fluid in fluids:
        result = run('external', 'grid', fluid)
        expected = reference / ('grid-' + fluid.encode().hex() + '-embedded.txt')
        assert result == expected.read_bytes(), 'External-only differs for ' + fluid
        outputs += len(re.findall(rb' = |first PropsSI ', result))
        errors += len(re.findall(rb': E ', result))
    for action in ['list', 'json', 'aliases', 'extras', 'threads', 'lifecycle']:
        result = run('external', action)
        assert result == (reference / (action + '-embedded.txt')).read_bytes(), 'External-only differs for ' + action
    symbols = subprocess.check_output(['nm', '--defined-only', str(build / 'libCoolProp.a')])
    assert b'gall_fluids_CBOR' not in symbols, 'External-only library embeds the combined blob'
    print(f'External-only: {len(fluids)} fluids, {outputs} numeric outputs, {errors} error records; baseline identical, combined blob absent', flush=True)
else:
    fluids = compare('list').decode().strip().split(',')
    outputs = 0
    errors = 0
    for i, fluid in enumerate(fluids, 1):
        result = compare('grid', fluid)
        outputs += len(re.findall(rb' = |first PropsSI ', result))
        errors += len(re.findall(rb': E ', result))
        if i % 16 == 0:
            print(f'Compared {i}/{len(fluids)} fluids', flush=True)
    for action in ['json', 'aliases', 'extras', 'threads', 'lifecycle']:
        result = compare(action)
        print(f'{action}: identical ({len(result)} bytes)', flush=True)
    print(f'Grid: {len(fluids)} fluids, {outputs} numeric outputs, {errors} error records; all text identical', flush=True)
for action in ['lazy', 'failure', 'recursive', 'threads']:
    print(run('external', action).decode(), end='', flush=True)
for mode in (['external'] if args.external_only else ['embedded', 'external']):
    rss = run(mode, 'rss').decode()
    (results / (mode + '-rss.txt')).write_text(rss)
    print(mode + ': ' + rss, end='', flush=True)
print('All checks passed', flush=True)
