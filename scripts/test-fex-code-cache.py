#!/usr/bin/env python3
"""Check the opt-in FEX code cache: identical results cold and warm, no reuse
for a same-named DLL with different bytes, safe handling of corrupt and
truncated databases, concurrent writers, and no JIT or self-modified code
in the cache."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from fex_modhost_contract import verify_patch_pins, verify_module_pin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--raven', required=True, type=Path)
p.add_argument('--client', required=True, type=Path)
p.add_argument('--library-a', required=True, type=Path)
p.add_argument('--library-b', required=True, type=Path)
p.add_argument('--modules', type=Path)
a = p.parse_args()

modules = a.modules or (Path(os.environ['RAVEN_FEX_MODULE_ROOT']) if os.environ.get('RAVEN_FEX_MODULE_ROOT') else None)
if modules is None or not (modules / 'xtajit64.dll').is_file():
    print('SKIP: provision RAVEN_FEX_MODULE_ROOT/xtajit64.dll', flush=True)
    sys.exit(77)
verify_module_pin(modules / 'xtajit64.dll', verify_patch_pins())

STATS = re.compile(r'FEX code cache pid=\d+ hits=(\d+) misses=(\d+) stores=(\d+) rejected=(\d+) skipped=(\d+) damaged=(\d+)')
# mov eax, imm32 for the JIT buffer, the self-modified function, and that function unmodified.
JIT, PATCHED, ORIGINAL = (bytes([0xb8, n, 0xde, 0xc0, 0x5e]) for n in (1, 2, 0))

with tempfile.TemporaryDirectory(prefix='raven-code-cache-test-') as temp:
    temp = Path(temp)
    apps = {}
    for name, library in (('a', a.library_a), ('b', a.library_b)):
        apps[name] = temp / ('app-' + name)
        apps[name].mkdir()
        shutil.copy2(a.client, apps[name] / 'codecache-client.exe')
        shutil.copy2(library, apps[name] / 'codecache_lib.dll')
    assert (apps['a'] / 'codecache_lib.dll').stat().st_size == (apps['b'] / 'codecache_lib.dll').stat().st_size
    runs = 0

    def start(variant, cache):
        global runs
        runs += 1
        prefix = temp / f'prefix-{runs}'
        prefix.mkdir()
        env = dict(os.environ, RAVEN_PREFIX=str(prefix), RAVEN_FEX_MODULE_ROOT=str(modules), RAVEN_FEX_CODE_CACHE_STATS='1')
        if cache:
            env.update(RAVEN_FEX_CODE_CACHE='1', RAVEN_FEX_CODE_CACHE_DIR=str(cache))
        else:
            env.pop('RAVEN_FEX_CODE_CACHE', None)
        return subprocess.Popen([str(a.raven), '--run-process', '--execution=fex', '--timeout-ms=20000', '--stdio=inherit',
                                 str(apps[variant] / 'codecache-client.exe')], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def finish(process, label):
        output, _ = process.communicate(timeout=60)
        assert process.returncode == 0, f'{label}: guest exit {process.returncode}\n{output}'
        result = re.search(r'CODECACHE_RESULT=([0-9a-f]{8})', output)
        smc = re.search(r'CODECACHE_SMC=([0-9a-f]{8})', output)
        assert result and smc and smc[1] == '5ec0de02', f'{label}: missing result\n{output}'
        stats = STATS.search(output)
        stats = dict(zip(('hits', 'misses', 'stores', 'rejected', 'skipped', 'damaged'), map(int, stats.groups()))) if stats else None
        print(f'{label}: result={result[1]} stats={stats}', flush=True)
        return result[1], stats

    def run(variant, cache, label):
        return finish(start(variant, cache), label)

    def databases(cache):
        return sorted(cache.rglob('*.foz'))

    def main_file(cache):
        files = [f for f in databases(cache) if not f.name.endswith('_idx.foz')]
        assert len(files) == 1, files
        return files[0]

    # 1. Identical results without the cache, cold and warm.
    expected = {v: run(v, None, 'baseline-' + v)[0] for v in ('a', 'b')}
    assert expected['a'] != expected['b'], 'variants must compute different results'
    cache = temp / 'cache'
    result, cold = run('a', cache, 'cold')
    assert result == expected['a'] and cold['stores'] > 0 and cold['hits'] == 0, cold
    result, warm = run('a', cache, 'warm')
    assert result == expected['a'] and warm['hits'] >= cold['stores'] - 2 and warm['rejected'] == 0, warm
    a_only = temp / 'cache-a-only'
    shutil.copytree(cache, a_only)

    # 2. Same name, size and timestamp, different bytes: its blocks never hit.
    result, changed = run('b', cache, 'modified-dll')
    assert result == expected['b'], f'stale translation of variant a ran: {result}'
    assert changed['hits'] < warm['hits'] and changed['stores'] > 0, changed

    # 3. JIT and self-modified image code never reach the cache; unmodified image code does.
    contents = b''.join(f.read_bytes() for f in databases(cache))
    assert ORIGINAL in contents, 'positive control: unmodified image code should be cached'
    assert JIT not in contents, 'JIT code was cached'
    assert PATCHED not in contents, 'self-modified code was cached'

    # 4a. Flipped bytes inside stored blobs are rejected; results stay correct.
    damaged = temp / 'cache-flipped'
    shutil.copytree(cache, damaged)
    target = main_file(damaged)
    data = bytearray(target.read_bytes())
    for offset in range(64, len(data), 97):
        data[offset] ^= 0x5a
    target.write_bytes(bytes(data))
    result, flipped = run('a', damaged, 'flipped')
    assert result == expected['a'] and flipped['rejected'] > 0, flipped

    # 4b. A truncated main file marks the database damaged; Raven sets it aside and the next run starts clean.
    truncated = temp / 'cache-truncated'
    shutil.copytree(cache, truncated)
    target = main_file(truncated)
    target.write_bytes(target.read_bytes()[:target.stat().st_size * 3 // 5])
    result, cut = run('a', truncated, 'truncated')
    assert result == expected['a'] and cut['damaged'] == 1 and cut['stores'] == 0, cut
    result, rebuilt = run('a', truncated, 'after-truncation')
    assert result == expected['a'] and rebuilt['damaged'] == 0 and rebuilt['stores'] > 0 and rebuilt['hits'] == 0, rebuilt

    # 4c. A writer killed mid-append leaves a torn index tail: earlier records still hit, and the
    # next writer trims it so records appended after it stay reachable.
    torn = temp / 'cache-torn'
    shutil.copytree(a_only, torn)
    index = next(f for f in databases(torn) if f.name.endswith('_idx.foz'))
    whole = index.read_bytes()
    index.write_bytes(whole + whole[16:16 + 60])
    result, cut = run('a', torn, 'torn-tail')
    assert result == expected['a'] and cut['damaged'] == 0 and cut['hits'] >= cold['stores'] - 2, cut
    result, appended = run('b', torn, 'torn-tail-append')
    assert result == expected['b'] and appended['stores'] > 0 and appended['damaged'] == 0, appended
    result, reached = run('b', torn, 'torn-tail-reached')
    # Without the trim every record after the tail is unreachable and the hits would not rise.
    assert result == expected['b'] and reached['hits'] > appended['hits'] and reached['rejected'] == 0, reached

    # 4d. A garbage index reads as one torn record: nothing hits, and the index is rebuilt.
    garbage = temp / 'cache-garbage'
    shutil.copytree(cache, garbage)
    index = next(f for f in databases(garbage) if f.name.endswith('_idx.foz'))
    index.write_bytes(index.read_bytes()[:16] + b'\xff' * 4096)
    result, noise = run('a', garbage, 'garbage-index')
    assert result == expected['a'] and noise['hits'] == 0 and noise['damaged'] == 0, noise
    result, rebuilt = run('a', garbage, 'after-garbage-index')
    assert result == expected['a'] and rebuilt['misses'] == 0 and rebuilt['rejected'] == 0, rebuilt

    # 4e. A bad header disables the cache for one run; Raven sets the namespace aside.
    header = temp / 'cache-header'
    shutil.copytree(cache, header)
    target = main_file(header)
    target.write_bytes(b'\0' * 16 + target.read_bytes()[16:])
    process = start('a', header)
    output, _ = process.communicate(timeout=60)
    assert process.returncode == 0 and f'CODECACHE_RESULT={expected["a"]}' in output and not STATS.search(output), output
    print('bad-header: cache disabled for the run, result correct', flush=True)
    result, healed = run('a', header, 'after-bad-header')
    assert result == expected['a'] and healed['stores'] > 0 and healed['damaged'] == 0, healed

    # 5. Two processes populate one cache at once; a third reads it intact.
    shared = temp / 'cache-concurrent'
    first, second = start('a', shared), start('a', shared)
    for process, label in ((first, 'concurrent-1'), (second, 'concurrent-2')):
        result, both = finish(process, label)
        assert result == expected['a'] and both['damaged'] == 0, both
    result, reader = run('a', shared, 'after-concurrent')
    assert result == expected['a'] and reader['hits'] >= cold['stores'] - 2, reader
    assert reader['rejected'] == 0 and reader['damaged'] == 0, reader

print('PASS: cold/warm identical, changed DLL bytes not reused, corrupt/truncated/torn/garbage databases survived, '
      'concurrent writers, JIT and self-modified code never cached')
