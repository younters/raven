#!/usr/bin/env python3
"""Aggregate opt-in FEX translation attempts and invalidations from a Raven log.

Unique counts are summed per process, not deduplicated across processes. An
attempt repeated at one RIP can also be a compile race or code-buffer rollover.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
a = p.parse_args()
text = a.log.read_text(errors='replace')
images = {(int(pid), int(base, 16), int(size), name) for pid, base, size, name in
          re.findall(r'Raven image pid=(\d+) base=([0-9a-f]+) size=(\d+) module=(.*)', text)}
modules = defaultdict(Counter)
pages = defaultdict(Counter)
for pid, page, unique, repeated, recompiled in re.findall(
        r'Raven profile pid=(\d+) Raven translations page=([0-9a-f]+) unique=(\d+) repeated=(\d+) recompiled=(\d+)', text):
    address = int(page, 16)
    names = {name for image_pid, base, size, name in images if image_pid == int(pid) and base <= address < base + size}
    module = '|'.join(sorted(names)) if names else '<anonymous>'
    values = Counter(unique=int(unique), repeated=int(repeated), recompiled=int(recompiled))
    modules[module].update(values)
    pages[f'{module}@{page}'].update(values)
reasons = ['unused', 'protection', 'DEP', 'section-unmap', 'free', 'explicit-or-dirty', 'write-fault', 'untracked-write', 'cache-clear']
invalidations = Counter()
for reason, start, size, count in re.findall(
        r'Raven invalidation reason=(\d+) start=([0-9a-f]+) size=(\d+) count=(\d+)', text):
    invalidations[reasons[int(reason)]] += int(count)
totals = sum(modules.values(), Counter())
print(json.dumps(dict(totals=totals, modules=modules, invalidations=invalidations,
                     hottest_pages=sorted(pages.items(), key=lambda kv: kv[1]['repeated'], reverse=True)[:30]), indent=2))
